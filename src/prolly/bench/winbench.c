/*
 * winbench.c
 *	  Measures version control within the window against a real Postgres.
 *
 * The working set is a table t(id bigint primary key, k int, v int, pad
 * text) with a secondary index on k.  The harness is its only writer.
 *
 *	Dolt commit	(writers quiesced): row changes since the last commit come
 *				from logical decoding into the prolly tree; the pages they
 *				touched come from the WAL's block references and are read
 *				through the buffer manager into a commit image built on the
 *				previous one.  The window is then severed and what falls out
 *				is discarded.
 *	checkout	by image: write back the pages that differ between the
 *				working set and the target commit's image (pages changed
 *				since the working set's base image, plus the two images'
 *				diff), truncating relations that shrank.
 *				by SQL (baseline): replay the prolly diff as DELETE/INSERT.
 *	verify		the table's rows against the prolly tree at the target, and
 *				amcheck on both indexes.
 *
 *	--planned	keyframes and checkouts as the keyframe planner (kfplan.h)
 *				decides, calibrating itself from the costs given (measured
 *				by bench/kfbench) as it observes each move: a commit
 *				keeps an image only when the change since the last one
 *				would be cheaper to restore than to replay, and a checkout
 *				replays rows, or restores the target's image or its nearest
 *				imaged ancestor's and replays the rest, whichever is
 *				priced lower.  Each checkout prints the choice, its estimate
 *				and the time it took, and is verified like any other.
 * Output: one CSV line per commit and per checkout, then a summary.
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../test/test_util.h"
#include "pgstore.h"
#include "../image.h"
#include "../kfplan.h"

#define BLCK 8192
#define HEAP_PER_VM_PAGE 32672	/* heap blocks one visibility map page covers */
#define FORK_MAIN 0
#define FORK_VM 2
#define MAX_RELS 8
#define MAX_COMMITS 100000

typedef struct Opts
{
	const char *conninfo;
	const char *pgdata;
	const char *store;
	int64_t		rows;
	int			pad;
	int			commits;
	int			changes;
	int			txns;
	int			window;
	int			checkouts;
	int			branches;
	int			branch_commits;
	int			verify;			/* 0 none, 1 changed rows, 2 everything */
	bool		sql_baseline;
	bool		reuse;			/* keep the loaded table from a previous run */
	bool		final_verify;	/* compare every row at the end */
	bool		lifecycle;		/* no images: checkpoints, replay checkouts */
	int			bg_ms;			/* background checkpoint every this many ms */
	int64_t		bg_bytes;		/* ... or once this much WAL is undecoded */
	int			diffs;
	bool		sql_decode;		/* decode by SQL function calls, not a stream */
	bool		concurrent;		/* check commits against a concurrent writer */
	const char *outside;		/* distances to measure, e.g. "5,15,30" */
	int			bg_rows;		/* stream: apply once this many rows are pending */
	uint64_t	seed;
	const char *planned;		/* keyframe costs: row,replay_page,page,replay_fixed,restore_fixed,capture */
} Opts;

typedef struct Rel
{
	char		name[64];
	uint32_t	oid;
	uint32_t	filenode;
	ImgRelKind	kind;
	uint32_t	parent;
} Rel;

typedef struct CommitInfo
{
	PHash		hash;
	char		branch[32];
	int			seq;
} CommitInfo;

static Opts o = {
	"host=/tmp port=5499 user=postgres dbname=bench", NULL,
	"/home/dustin/pgv2-bench/store.dat", 200000, 100, 20, 2000, 10, 10, 10,
	0, 5, 1, true, false, true, false, 1000, 16 << 20, 20, false, false, NULL, 5000, 1, NULL
};

static PGconn *db;				/* the workload and queries */
static PGconn *dec;				/* logical decoding: the slot's one user */
static PGconn *rep;				/* replay, under a replication origin decoding skips */
static pthread_mutex_t repo_lock = PTHREAD_MUTEX_INITIALIZER;
static char working_lsn[32];	/* working root = working set as of here */
static volatile bool bg_stop;
static uint32_t dboid;
static Rel rels[MAX_RELS];
static int nrels;
static ChunkStore *cs;
static bool store_pg;			/* --store-pg: the history store in Postgres */
static ChunkStore *archive;		/* where severed history goes, with --outside */
static VcRepo repo;
static ImgFormat fmt;
static PHash base_img;			/* the image the working set's pages derive from */
static char base_lsn[32];		/* WAL position where they last equaled it */
static int64_t next_id;
static CommitInfo commits[MAX_COMMITS];
static int ncommits;
static uint64_t snap_dummy_xip[1];
static uint64_t last_dirty,
			last_diff;			/* checkout page sources, for reporting */
static KfPlanner *planner;		/* with --planned */
static uint64_t rows_since_image;	/* rows committed since the last image */
static double density = 1.0;	/* pages per row changed, from the images captured */

/* ----------------------------------------------------------------- utils */

static double
now_ms(void)
{
	return now_sec() * 1000.0;
}

static PGresult *
qpc(PGconn *c, const char *sql, int n, const char *const *vals, const int *lens,
	const int *fmts, int resfmt)
{
	PGresult   *r = PQexecParams(c, sql, n, NULL, vals, lens, fmts, resfmt);
	ExecStatusType st = PQresultStatus(r);

	if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK && st != PGRES_COPY_OUT)
	{
		fprintf(stderr, "\nquery failed: %s\n%s\n", sql, PQerrorMessage(c));
		exit(1);
	}
	return r;
}

static PGresult *
qp(const char *sql, int n, const char *const *vals, const int *lens,
   const int *fmts, int resfmt)
{
	return qpc(db, sql, n, vals, lens, fmts, resfmt);
}

static PGresult *
q(const char *sql)
{
	return qp(sql, 0, NULL, NULL, NULL, 0);
}

static void
qx(const char *sql)
{
	PQclear(q(sql));
}

/*
 * The WAL position everything so far is flushed to.  Read-only work can
 * insert WAL that nothing flushes (a hint written as a full page image, such
 * as btree marking an entry dead), and pg_get_wal_block_info reads only up
 * to the flushed position, so force a flush first.
 */
static void
flush_lsn_on(PGconn *c, char *out)
{
	PGresult   *r = qpc(c, "SELECT pg_logical_emit_message(false, 'prolly', '', true), "
						"pg_current_wal_flush_lsn()", 0, NULL, NULL, NULL, 0);

	snprintf(out, 32, "%s", PQgetvalue(r, 0, 1));
	PQclear(r);
}

static void
flush_lsn(char *out)
{
	flush_lsn_on(db, out);
}

static void
pad_for(char *buf, int64_t id, int pad)
{
	char		digits[24];
	int			n = snprintf(digits, sizeof(digits), "%lld", (long long) id);
	int			fill = pad > n ? pad - n : 0;

	memset(buf, 'x', (size_t) fill);
	memcpy(buf + fill, digits, (size_t) (pad > n ? n : pad));
	buf[pad] = '\0';
}

/* Row codec for the prolly tree: key orders like the bigint, value k v pad. */
static void
row_key(uint8_t *k, int64_t id)
{
	put_u64(k, (uint64_t) id ^ (1ULL << 63));
}

static size_t
row_val(uint8_t *out, int32_t k, int32_t v, const char *pad, size_t padlen)
{
	put_u64(out, ((uint64_t) (uint32_t) k << 32) | (uint32_t) v);
	memcpy(out + 8, pad, padlen);
	return 8 + padlen;
}

static void
normalize(uint8_t *page)
{
	/* pd_lsn and pd_checksum change with every write; they are not content */
	memset(page, 0, 10);
}

static const char *
hex(const PHash *h)
{
	static char buf[4][PHASH_HEX_LEN + 1];
	static int	i;

	i = (i + 1) % 4;
	phash_hex(h, buf[i]);
	buf[i][12] = '\0';
	return buf[i];
}

/* ------------------------------------------------------------------ setup */

static void
load_rels(void)
{
	PGresult   *r = q("SELECT c.oid, c.relname, c.relfilenode, c.relkind, "
					  "  coalesce((SELECT i.indrelid FROM pg_index i WHERE i.indexrelid = c.oid), 0) "
					  "FROM pg_class c "
					  "WHERE c.oid IN ('t'::regclass, 't_pkey'::regclass, 't_k'::regclass, "
					  "  (SELECT reltoastrelid FROM pg_class WHERE oid = 't'::regclass), "
					  "  (SELECT indexrelid FROM pg_index WHERE indrelid = "
					  "     (SELECT reltoastrelid FROM pg_class WHERE oid = 't'::regclass))) "
					  "ORDER BY c.oid");

	nrels = PQntuples(r);
	CHECK(nrels <= MAX_RELS);
	for (int i = 0; i < nrels; i++)
	{
		char		kind = PQgetvalue(r, i, 3)[0];

		rels[i].oid = (uint32_t) strtoul(PQgetvalue(r, i, 0), NULL, 10);
		snprintf(rels[i].name, sizeof(rels[i].name), "%s", PQgetvalue(r, i, 1));
		rels[i].filenode = (uint32_t) strtoul(PQgetvalue(r, i, 2), NULL, 10);
		rels[i].kind = kind == 'i' ? IMG_REL_INDEX : kind == 't' ? IMG_REL_TOAST : IMG_REL_TABLE;
		rels[i].parent = (uint32_t) strtoul(PQgetvalue(r, i, 4), NULL, 10);
	}
	PQclear(r);
	r = q("SELECT oid FROM pg_database WHERE datname = current_database()");
	dboid = (uint32_t) strtoul(PQgetvalue(r, 0, 0), NULL, 10);
	PQclear(r);
}

static Rel *
rel_by_filenode(uint32_t fn)
{
	for (int i = 0; i < nrels; i++)
		if (rels[i].filenode == fn)
			return &rels[i];
	return NULL;
}

static uint32_t
rel_nblocks(const Rel *r, int fork)
{
	char		sql[160];
	PGresult   *res;
	uint32_t	n;

	snprintf(sql, sizeof(sql), "SELECT pg_relation_size(%u, '%s') / %d",
			 r->oid, fork == FORK_VM ? "vm" : "main", BLCK);
	res = q(sql);
	n = (uint32_t) strtoul(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	return n;
}

static void
setup_db(void)
{
	char		sql[512];
	double		t = now_ms();
	int64_t		step = 5000000;

	qx("SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots WHERE slot_name = 'prolly'");
	if (!o.reuse)
	{
		qx("DROP TABLE IF EXISTS t");
		qx("CREATE TABLE t (id bigint, k int NOT NULL, v int NOT NULL, pad text NOT NULL) "
		   "WITH (autovacuum_enabled = false, toast.autovacuum_enabled = false)");
		for (int64_t a = 1; a <= o.rows; a += step)
		{
			int64_t		b = a + step - 1 < o.rows ? a + step - 1 : o.rows;

			snprintf(sql, sizeof(sql),
					 "INSERT INTO t SELECT g, (g * 7919) %% 1000003, 0, lpad(g::text, %d, 'x') "
					 "FROM generate_series(%lld::bigint, %lld::bigint) g", o.pad, (long long) a, (long long) b);
			qx(sql);
		}
		qx("ALTER TABLE t ADD PRIMARY KEY (id)");
		qx("CREATE INDEX t_k ON t (k)");
		qx("VACUUM (FREEZE, ANALYZE) t");
	}
	qx("CHECKPOINT");
	qx("SELECT pg_create_logical_replication_slot('prolly', 'test_decoding')");
	load_rels();
	{
		PGresult   *r = q("SELECT coalesce(max(id), 0) + 1 FROM t");

		next_id = strtoll(PQgetvalue(r, 0, 0), NULL, 10);
		PQclear(r);
	}
	printf("# loaded %lld rows in %.1fs\n", (long long) (next_id - 1), (now_ms() - t) / 1000);
}

/* ------------------------------------------------------------ prolly side */

static PHash
table_root(const PHash *rv)
{
	VcTable		t;

	CHECK(vc_table_get(&repo.ctx, rv, "t", &t));
	return t.data;
}

/* The initial table in the prolly tree: the same rows the load generated. */
static void
load_prolly(void)
{
	const int64_t batch = 1000000;
	uint8_t    *keys = px_malloc((size_t) batch * 8);
	uint8_t    *vals = px_malloc((size_t) batch * (8 + (size_t) o.pad));
	ProllyEdit *e = px_malloc((size_t) batch * sizeof(ProllyEdit));
	char	   *padbuf = px_malloc((size_t) o.pad + 1);
	PHash		rv = vc_working(&repo),
				data;
	const char *ddl = "CREATE TABLE t (id bigint PRIMARY KEY, k int, v int, pad text)";
	double		t0 = now_ms();

	CHECK(vc_table_create(&repo.ctx, &rv, "t", ddl, strlen(ddl), &rv) == 0);
	data = table_root(&rv);
	for (int64_t a = 1; a < next_id; a += batch)
	{
		size_t		n = 0;

		for (int64_t id = a; id < next_id && id < a + batch; id++, n++)
		{
			size_t		vl;

			pad_for(padbuf, id, o.pad);
			row_key(keys + n * 8, id);
			vl = row_val(vals + n * (8 + (size_t) o.pad), (int32_t) ((id * 7919) % 1000003), 0,
						 padbuf, (size_t) o.pad);
			e[n] = (ProllyEdit) {keys + n * 8, 8, vals + n * (8 + (size_t) o.pad), vl, false};
		}
		CHECK_OK(prolly_apply(&repo.ctx, &data, e, n, &data));
	}
	CHECK(vc_table_set_data(&repo.ctx, &rv, "t", &data, &rv) == 0);
	vc_set_working(&repo, &rv);
	free(keys);
	free(vals);
	free(e);
	free(padbuf);
	printf("# prolly table built in %.1fs\n", (now_ms() - t0) / 1000);
}

/* Parses one test_decoding line for table t into an edit; false otherwise. */
typedef struct EditList
{
	ProllyEdit *e;
	size_t		n,
				cap;
	Buf			arena;
	size_t	   *koff,
			   *voff;
} EditList;

static const char *
field(const char *s, const char *name)
{
	const char *p = strstr(s, name);

	return p ? strchr(p, ':') + 1 : NULL;
}

static void
parse_change(const char *line, EditList *el)
{
	const char *op;
	int64_t		id;
	uint8_t		key[8];
	bool		del;

	if (strncmp(line, "table public.t: ", 16) != 0)
		return;
	op = line + 16;
	del = strncmp(op, "DELETE", 6) == 0;
	id = strtoll(field(op, "id[bigint]"), NULL, 10);
	row_key(key, id);
	px_grow((void **) &el->e, &el->cap, el->n + 1, sizeof(ProllyEdit));
	el->koff = px_realloc(el->koff, el->cap * sizeof(size_t));
	el->voff = px_realloc(el->voff, el->cap * sizeof(size_t));
	el->koff[el->n] = el->arena.len;
	buf_append(&el->arena, key, 8);
	el->voff[el->n] = el->arena.len;
	el->e[el->n].del = del;
	el->e[el->n].vlen = 0;
	if (!del)
	{
		int32_t		k = (int32_t) strtol(field(op, "k[integer]"), NULL, 10);
		int32_t		v = (int32_t) strtol(field(op, "v[integer]"), NULL, 10);
		const char *ps = field(op, "pad[text]");
		Buf			pad = BUF_INIT;
		uint8_t		head[8];

		CHECK(ps && *ps == '\'');
		for (ps++; *ps; ps++)
		{
			if (*ps == '\'')
			{
				if (ps[1] == '\'')
				{
					buf_append_byte(&pad, '\'');
					ps++;
					continue;
				}
				break;
			}
			buf_append_byte(&pad, (uint8_t) *ps);
		}
		row_val(head, k, v, "", 0);
		buf_append(&el->arena, head, 8);
		buf_append(&el->arena, pad.data, pad.len);
		el->e[el->n].vlen = 8 + pad.len;
		buf_free(&pad);
	}
	el->n++;
}

/* Logical decoding up to lsn, into the working table; returns rows changed. */
static size_t
decode_into_prolly(const char *lsn)
{
	const char *vals[1] = {lsn};
	PGresult   *r = qpc(o.lifecycle ? dec : db,
						"SELECT data FROM pg_logical_slot_get_changes('prolly', $1::pg_lsn, NULL, "
						"'include-xids', '0', 'skip-empty-xacts', '1', 'only-local', '1')",
						1, vals, NULL, NULL, 0);
	EditList	el = {0};
	size_t		n;

	for (int i = 0; i < PQntuples(r); i++)
		parse_change(PQgetvalue(r, i, 0), &el);
	PQclear(r);
	for (size_t i = 0; i < el.n; i++)
	{
		el.e[i].key = el.arena.data + el.koff[i];
		el.e[i].klen = 8;
		el.e[i].val = el.arena.data + el.voff[i];
	}
	if (el.n > 0)
	{
		PHash		rv = vc_working(&repo);

		CHECK(vc_table_edit(&repo.ctx, &rv, "t", el.e, el.n, &rv) == 0);
		vc_set_working(&repo, &rv);
	}
	n = el.n;
	free(el.e);
	free(el.koff);
	free(el.voff);
	buf_free(&el.arena);
	return n;
}

/* Applies an edit list to the working table and empties it; caller holds repo_lock. */
static size_t
editlist_apply(EditList *el)
{
	size_t		n = el->n;

	for (size_t i = 0; i < el->n; i++)
	{
		el->e[i].key = el->arena.data + el->koff[i];
		el->e[i].klen = 8;
		el->e[i].val = el->arena.data + el->voff[i];
	}
	if (el->n > 0)
	{
		PHash		rv = vc_working(&repo);

		CHECK(vc_table_edit(&repo.ctx, &rv, "t", el->e, el->n, &rv) == 0);
		vc_set_working(&repo, &rv);
	}
	el->n = 0;
	el->arena.len = 0;
	return n;
}

/* Moves every edit of src to the end of dst. */
static void
editlist_move(EditList *dst, EditList *src)
{
	for (size_t i = 0; i < src->n; i++)
	{
		px_grow((void **) &dst->e, &dst->cap, dst->n + 1, sizeof(ProllyEdit));
		dst->koff = px_realloc(dst->koff, dst->cap * sizeof(size_t));
		dst->voff = px_realloc(dst->voff, dst->cap * sizeof(size_t));
		dst->koff[dst->n] = dst->arena.len;
		buf_append(&dst->arena, src->arena.data + src->koff[i], 8);
		dst->voff[dst->n] = dst->arena.len;
		buf_append(&dst->arena, src->arena.data + src->voff[i], src->e[i].vlen);
		dst->e[dst->n].del = src->e[i].del;
		dst->e[dst->n].vlen = src->e[i].vlen;
		dst->n++;
	}
	src->n = 0;
	src->arena.len = 0;
}

/* ------------------------------------------------------------ page sets */

typedef struct PageRef
{
	uint32_t	oid;
	int			fork;
	uint32_t	block;
} PageRef;

typedef struct PageSet
{
	PageRef    *v;
	size_t		n,
				cap;
} PageSet;

static void
pageset_add(PageSet *s, uint32_t oid, int fork, uint32_t block)
{
	px_grow((void **) &s->v, &s->cap, s->n + 1, sizeof(PageRef));
	s->v[s->n++] = (PageRef) {oid, fork, block};
}

static int
pageref_cmp(const void *a, const void *b)
{
	const PageRef *x = a,
			   *y = b;

	if (x->oid != y->oid)
		return x->oid < y->oid ? -1 : 1;
	if (x->fork != y->fork)
		return x->fork - y->fork;
	return (x->block > y->block) - (x->block < y->block);
}

static void
pageset_sort(PageSet *s)
{
	size_t		k = 0;

	if (s->n > 1)
		qsort(s->v, s->n, sizeof(PageRef), pageref_cmp);
	for (size_t i = 0; i < s->n; i++)
		if (k == 0 || pageref_cmp(&s->v[k - 1], &s->v[i]) != 0)
			s->v[k++] = s->v[i];
	s->n = k;
}

static uint64_t
lsn_value(const char *lsn)
{
	unsigned int hi,
				lo;

	CHECK(sscanf(lsn, "%X/%X", &hi, &lo) == 2);
	return ((uint64_t) hi << 32) | lo;
}

/* Pages the WAL says changed between two positions, with their VM pages. */
static void
wal_pages(const char *from, const char *to, PageSet *s)
{
	char		db_s[16],
				fns[256];
	const char *vals[4] = {from, to, db_s, fns};
	PGresult   *r;
	size_t		len = 0;

	if (lsn_value(from) >= lsn_value(to))
		return;					/* nothing written in between */
	snprintf(db_s, sizeof(db_s), "%u", dboid);
	len += (size_t) snprintf(fns + len, sizeof(fns) - len, "{");
	for (int i = 0; i < nrels; i++)
		len += (size_t) snprintf(fns + len, sizeof(fns) - len, "%s%u", i ? "," : "", rels[i].filenode);
	snprintf(fns + len, sizeof(fns) - len, "}");
	r = qp("SELECT DISTINCT relfilenode, relforknumber, relblocknumber "
		   "FROM pg_get_wal_block_info($1::pg_lsn, $2::pg_lsn, false) "
		   "WHERE reldatabase = $3::oid AND relfilenode = ANY($4::oid[])",
		   4, vals, NULL, NULL, 0);
	for (int i = 0; i < PQntuples(r); i++)
	{
		Rel		   *rel = rel_by_filenode((uint32_t) strtoul(PQgetvalue(r, i, 0), NULL, 10));
		int			fork = atoi(PQgetvalue(r, i, 1));
		uint32_t	blk = (uint32_t) strtoul(PQgetvalue(r, i, 2), NULL, 10);

		if (fork != FORK_MAIN && fork != FORK_VM)
			continue;			/* the free space map is not captured */
		pageset_add(s, rel->oid, fork, blk);
		/* a heap change can clear a visibility map bit without naming its page */
		if (fork == FORK_MAIN && (rel->kind == IMG_REL_TABLE || rel->kind == IMG_REL_TOAST))
			pageset_add(s, rel->oid, FORK_VM, blk / HEAP_PER_VM_PAGE);
	}
	PQclear(r);
	pageset_sort(s);
}

/*
 * Sets the working set's base point: its pages equal base_img as of now.
 * With wal_log_hints on, the first change to any page after a checkpoint
 * is logged, even a hint the server would otherwise write silently (such
 * as btree marking an entry dead).  Checkpointing here makes every page
 * change from now on visible in the WAL, which is what makes "pages
 * changed since the base" exact.
 */
static void
set_base_point(const PHash *img)
{
	base_img = *img;
	flush_lsn(base_lsn);
	qx("CHECKPOINT");
}

/* ------------------------------------------------------------- capture */

static ImgRelation
descriptor(const Rel *r, const uint32_t *nblocks)
{
	ImgRelation d;

	memset(&d, 0, sizeof(d));
	d.relid = r->oid;
	d.kind = r->kind;
	d.parent = r->parent;
	d.name = r->name;
	d.namelen = strlen(r->name);
	d.nblocks[FORK_MAIN] = nblocks[FORK_MAIN];
	d.nblocks[FORK_VM] = nblocks[FORK_VM];
	return d;
}

static const ImgSnapshot no_snapshot = {0, 0, snap_dummy_xip, 0};

/* The first image: every page, read from the files after a checkpoint. */
static PHash
capture_full(const PHash *commit, uint64_t *pages_out)
{
	ImgBuilder *b;
	PHash		img;
	uint8_t    *buf = px_malloc(BLCK * 128);
	uint64_t	pages = 0;

	qx("CHECKPOINT");
	CHECK_OK(img_begin(&repo.ctx, &fmt, NULL, &b));
	for (int i = 0; i < nrels; i++)
	{
		uint32_t	nb[IMG_NFORKS] = {0};
		char		sql[96];
		PGresult   *r;
		char		rpath[512];
		ImgRelation d;

		snprintf(sql, sizeof(sql), "SELECT pg_relation_filepath(%u)", rels[i].oid);
		r = q(sql);
		snprintf(rpath, sizeof(rpath), "%s/%s", o.pgdata, PQgetvalue(r, 0, 0));
		PQclear(r);
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
		{
			uint32_t	blk = 0;

			for (int seg = 0;; seg++)
			{
				char		path[600];
				int			fd;
				ssize_t		got;

				snprintf(path, sizeof(path), "%s%s%s", rpath, f == FORK_VM ? "_vm" : "",
						 seg ? "" : "");
				if (seg)
					snprintf(path + strlen(path), sizeof(path) - strlen(path), ".%d", seg);
				fd = open(path, O_RDONLY);
				if (fd < 0)
					break;
				while ((got = read(fd, buf, BLCK * 128)) > 0)
				{
					CHECK(got % BLCK == 0);
					for (ssize_t off = 0; off < got; off += BLCK, blk++, pages++)
					{
						normalize(buf + off);
						CHECK_OK(img_put_page(b, rels[i].oid, f, blk, buf + off));
					}
				}
				close(fd);
			}
			nb[f] = blk;
		}
		d = descriptor(&rels[i], nb);
		CHECK_OK(img_put_relation(b, &d));
	}
	CHECK_OK(img_finish(b, commit, 0, &no_snapshot, &img));
	free(buf);
	*pages_out = pages;
	return img;
}

/* Reads pages through the buffer manager, batch by batch, into a builder. */
static void
read_pages(ImgBuilder *b, const Rel *rel, int fork, const uint32_t *blocks, size_t n)
{
	const size_t batch = 2000;
	Buf			arr = BUF_INIT;

	for (size_t a = 0; a < n; a += batch)
	{
		size_t		m = n - a < batch ? n - a : batch;
		const char *vals[3];
		char		oid_s[16];
		PGresult   *r;

		arr.len = 0;
		buf_append_byte(&arr, '{');
		for (size_t i = 0; i < m; i++)
		{
			char		num[16];
			int			len = snprintf(num, sizeof(num), "%s%u", i ? "," : "", blocks[a + i]);

			buf_append(&arr, num, (size_t) len);
		}
		buf_append(&arr, "}", 2);
		snprintf(oid_s, sizeof(oid_s), "%u", rel->oid);
		vals[0] = oid_s;
		vals[1] = fork == FORK_VM ? "vm" : "main";
		vals[2] = (const char *) arr.data;
		r = qp("SELECT get_raw_page($1::oid::regclass::text, $2, b) "
			   "FROM unnest($3::int8[]) WITH ORDINALITY u(b, o) ORDER BY o",
			   3, vals, NULL, NULL, 1);
		CHECK((size_t) PQntuples(r) == m);
		for (size_t i = 0; i < m; i++)
		{
			uint8_t    *page = (uint8_t *) PQgetvalue(r, (int) i, 0);

			CHECK(PQgetlength(r, (int) i, 0) == BLCK);
			normalize(page);
			CHECK_OK(img_put_page(b, rel->oid, fork, blocks[a + i], page));
		}
		PQclear(r);
	}
	buf_free(&arr);
}

/* An image built on base: the pages the WAL says changed since base. */
static PHash
capture_incremental(const PHash *commit, const char *lsn, uint64_t *pages_out)
{
	PageSet		s = {0};
	ImgBuilder *b;
	PHash		img;
	uint32_t   *blocks = NULL;
	size_t		bcap = 0;

	wal_pages(base_lsn, lsn, &s);
	CHECK_OK(img_begin(&repo.ctx, &fmt, &base_img, &b));
	for (int i = 0; i < nrels; i++)
	{
		uint32_t	nb[IMG_NFORKS] = {0};
		ImgRelation d;

		nb[FORK_MAIN] = rel_nblocks(&rels[i], FORK_MAIN);
		nb[FORK_VM] = rel_nblocks(&rels[i], FORK_VM);
		d = descriptor(&rels[i], nb);
		CHECK_OK(img_put_relation(b, &d));
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
		{
			size_t		n = 0;

			for (size_t j = 0; j < s.n; j++)
				if (s.v[j].oid == rels[i].oid && s.v[j].fork == f && s.v[j].block < nb[f])
				{
					px_grow((void **) &blocks, &bcap, n + 1, sizeof(uint32_t));
					blocks[n++] = s.v[j].block;
				}
			read_pages(b, &rels[i], f, blocks, n);
		}
	}
	CHECK_OK(img_finish(b, commit, 0, &no_snapshot, &img));
	*pages_out = s.n;
	free(blocks);
	free(s.v);
	return img;
}

/* -------------------------------------------------------------- workload */

static void
run_ids(const char *sql, const int64_t *ids, int n, int seq)
{
	Buf			arr = BUF_INIT;
	char		seq_s[16],
				pad_s[16];
	const char *vals[3];

	buf_append_byte(&arr, '{');
	for (int i = 0; i < n; i++)
	{
		char		num[24];
		int			len = snprintf(num, sizeof(num), "%s%lld", i ? "," : "", (long long) ids[i]);

		buf_append(&arr, num, (size_t) len);
	}
	buf_append(&arr, "}", 2);
	snprintf(seq_s, sizeof(seq_s), "%d", seq);
	snprintf(pad_s, sizeof(pad_s), "%d", o.pad);
	vals[0] = (const char *) arr.data;
	vals[1] = seq_s;
	vals[2] = pad_s;
	PQclear(qp(sql, 3, vals, NULL, NULL, 0));
	buf_free(&arr);
}

/* o.changes row changes in o.txns transactions: updates, inserts, deletes. */
static void
workload(int seq)
{
	int			per = o.changes / o.txns;
	int64_t    *ids = px_malloc((size_t) per * sizeof(int64_t));

	for (int t = 0; t < o.txns; t++)
	{
		int			nu = per * 5 / 10,
					np = per * 1 / 10,
					ni = per * 2 / 10,
					nd = per - nu - np - ni;

		qx("BEGIN");
		for (int i = 0; i < nu; i++)
			ids[i] = 1 + (int64_t) rnd_below((uint64_t) (next_id - 1));
		run_ids("UPDATE t SET k = (id * 31 + $2::int) % 1000003, v = $2::int "
				"WHERE id = ANY($1::bigint[]) AND $3::int > 0", ids, nu, seq);
		for (int i = 0; i < np; i++)
			ids[i] = 1 + (int64_t) rnd_below((uint64_t) (next_id - 1));
		run_ids("UPDATE t SET pad = lpad('u' || id || '.' || $2::text, $3::int, 'y'), v = $2::int "
				"WHERE id = ANY($1::bigint[])", ids, np, seq);
		for (int i = 0; i < ni; i++)
			ids[i] = next_id++;
		run_ids("INSERT INTO t SELECT g, (g * 7919) % 1000003, $2::int, lpad(g::text, $3::int, 'x') "
				"FROM unnest($1::bigint[]) g", ids, ni, seq);
		for (int i = 0; i < nd; i++)
			ids[i] = 1 + (int64_t) rnd_below((uint64_t) (next_id - 1));
		run_ids("DELETE FROM t WHERE id = ANY($1::bigint[]) AND $2::int >= 0 AND $3::int > 0",
				ids, nd, seq);
		qx("COMMIT");
	}
	free(ids);
}

/* --------------------------------------------------------------- commit */

static void
dolt_commit(int seq, const char *branch)
{
	char		lsn[32],
				msg[64];
	double		t0 = now_ms(),
				t1,
				t2,
				t3;
	size_t		rows;
	uint64_t	pages,
				new0 = cs->stats.new_chunks,
				bytes0 = cs->stats.new_bytes;
	PHash		c,
				img;
	uint32_t	seg;

	flush_lsn(lsn);
	rows = decode_into_prolly(lsn);
	snprintf(msg, sizeof(msg), "%s %d", branch, seq);
	c = vc_commit(&repo, msg);
	t1 = now_ms();
	rows_since_image += rows;
	pages = 0;
	if (planner)
	{
		PageSet		s = {0};

		wal_pages(base_lsn, lsn, &s);
		pages = s.n;
		free(s.v);
	}
	if (planner == NULL ||
		planner->ops->should_capture(planner, &(KfMove) {rows_since_image, pages, 0}))
	{
		img = capture_incremental(&c, lsn, &pages);
		CHECK(vc_image_set(&repo, &c, &img) == VC_OK);
		if (rows_since_image > 0)
			density = 0.8 * density + 0.2 * ((double) pages / (double) rows_since_image);
		rows_since_image = 0;
		t2 = now_ms();
		set_base_point(&img);
	}
	else
	{
		/* no image: the working set's base stays the last one */
		printf("# commit %s %d: no keyframe (%zu rows on %llu pages since the last)\n",
			   branch, seq, (size_t) rows_since_image, (unsigned long long) pages);
		pages = 0;
		t2 = now_ms();
	}
	t3 = now_ms();

	/* the window: what falls out is discarded, images with it */
	CHECK(vc_sever(&repo, &(VcWindow) {.depth = (uint32_t) o.window}, &seg) == VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK(vc_segment_discard(&repo, seg) == VC_OK);

	CHECK(ncommits < MAX_COMMITS);
	commits[ncommits].hash = c;
	snprintf(commits[ncommits].branch, sizeof(commits[ncommits].branch), "%s", branch);
	commits[ncommits].seq = seq;
	ncommits++;
	printf("commit,%s,%d,%s,rows=%zu,pages=%llu,img_new_chunks=%llu,img_new_mb=%.1f,"
		   "prolly_ms=%.0f,image_ms=%.0f,checkpoint_ms=%.0f,store_live_mb=%.0f\n",
		   branch, seq, hex(&c), rows, (unsigned long long) pages,
		   (unsigned long long) (cs->stats.new_chunks - new0),
		   (cs->stats.new_bytes - bytes0) / 1048576.0, t1 - t0, t2 - t1, t3 - t2,
		   cs->ops->bytes(cs) / 1048576.0);
	fflush(stdout);
}

/* ------------------------------------------------------------- checkout */

typedef struct DiffCollect
{
	PageSet    *s;
} DiffCollect;

static void
img_diff_fn(void *arg, uint32_t relid, int fork, uint32_t block,
			const PHash *from, const PHash *to)
{
	pageset_add(arg, relid, fork, block);
}

static void
write_pages(const Rel *rel, int fork, const ImgHeader *target, const uint32_t *blocks,
			size_t n)
{
	const size_t batch = 2000;
	Buf			arr = BUF_INIT,
				pages = BUF_INIT;

	for (size_t a = 0; a < n; a += batch)
	{
		size_t		m = n - a < batch ? n - a : batch;
		const char *vals[4];
		int			lens[4] = {0, 0, 0, 0};
		int			fmts[4] = {0, 0, 0, 1};
		char		oid_s[16];

		arr.len = 0;
		pages.len = 0;
		buf_append_byte(&arr, '{');
		for (size_t i = 0; i < m; i++)
		{
			char		num[16];
			int			len = snprintf(num, sizeof(num), "%s%u", i ? "," : "", blocks[a + i]);
			const uint8_t *p;

			buf_append(&arr, num, (size_t) len);
			if (blocks[a + i] < (fork == FORK_MAIN ? UINT32_MAX : UINT32_MAX) &&
				img_page(&repo.ctx, target, rel->oid, fork, blocks[a + i], &p) == PROLLY_OK)
				buf_append(&pages, p, BLCK);
			else
			{
				uint8_t		zero[BLCK] = {0};

				buf_append(&pages, zero, BLCK);	/* map pages past the image's end */
			}
		}
		buf_append(&arr, "}", 2);
		snprintf(oid_s, sizeof(oid_s), "%u", rel->oid);
		vals[0] = oid_s;
		vals[1] = fork == FORK_VM ? "vm" : "main";
		vals[2] = (const char *) arr.data;
		vals[3] = (const char *) pages.data;
		lens[3] = (int) pages.len;
		PQclear(qp("SELECT prolly_put_pages($1::oid::regclass, $2, $3::int4[], $4::bytea)",
				   4, vals, lens, fmts, 0));
	}
	buf_free(&arr);
	buf_free(&pages);
}

/* Moves the working set to target by writing pages; returns pages written. */
static uint64_t
checkout_images(const PHash *target)
{
	PHash		timg;
	ImgHeader	th,
				bh;
	PageSet		s = {0};
	char		lsn[32];
	uint64_t	written = 0;
	uint32_t   *blocks = NULL;
	size_t		bcap = 0;

	CHECK(vc_image_get(&repo, target, &timg));
	CHECK_OK(img_load(&repo.ctx, &timg, &th));
	CHECK_OK(img_load(&repo.ctx, &base_img, &bh));
	flush_lsn(lsn);
	wal_pages(base_lsn, lsn, &s);	/* what the working set changed since its base */
	last_dirty = s.n;
	{
		size_t		before = s.n;

		CHECK_OK(img_diff(&repo.ctx, &bh, &th, img_diff_fn, &s));
		last_diff = s.n - before;
	}
	pageset_sort(&s);

	qx("BEGIN");
	qx("LOCK TABLE t IN ACCESS EXCLUSIVE MODE");
	for (int i = 0; i < nrels; i++)
	{
		ImgRelation d;
		uint32_t	cur_main = rel_nblocks(&rels[i], FORK_MAIN);
		uint32_t	cur_vm = rel_nblocks(&rels[i], FORK_VM);

		CHECK_OK(img_relation(&repo.ctx, &th, rels[i].oid, &d));
		if (cur_main > d.nblocks[FORK_MAIN])
		{
			char		sql[96];

			snprintf(sql, sizeof(sql), "SELECT prolly_truncate(%u, %u)", rels[i].oid,
					 d.nblocks[FORK_MAIN]);
			qx(sql);
			cur_vm = rel_nblocks(&rels[i], FORK_VM);
		}
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
		{
			size_t		n = 0;

			for (size_t j = 0; j < s.n; j++)
				if (s.v[j].oid == rels[i].oid && s.v[j].fork == f &&
					s.v[j].block < d.nblocks[f])
				{
					px_grow((void **) &blocks, &bcap, n + 1, sizeof(uint32_t));
					blocks[n++] = s.v[j].block;
				}
			/* map pages the image does not have are cleared */
			if (f == FORK_VM)
				for (uint32_t blk = d.nblocks[FORK_VM]; blk < cur_vm; blk++)
				{
					px_grow((void **) &blocks, &bcap, n + 1, sizeof(uint32_t));
					blocks[n++] = blk;
				}
			write_pages(&rels[i], f, &th, blocks, n);
			written += n;
		}
	}
	qx("COMMIT");
	qx("SELECT pg_replication_slot_advance('prolly', pg_current_wal_flush_lsn())");
	set_base_point(&timg);
	img_header_free(&th);
	img_header_free(&bh);
	free(blocks);
	free(s.v);
	return written;
}

typedef struct RowDiffs
{
	Buf			del,
				ins;			/* text arrays being built */
	size_t		ndel,
				nins;
	Buf			k,
				v,
				pad;
} RowDiffs;

static void
sql_diff_fn(void *arg, const ProllyDiff *d)
{
	RowDiffs   *rd = arg;
	char		num[64];
	int64_t		id = (int64_t) (get_u64(d->key) ^ (1ULL << 63));
	int			len;

	if (d->to == NULL)
	{
		len = snprintf(num, sizeof(num), "%s%lld", rd->ndel ? "," : "", (long long) id);
		buf_append(&rd->del, num, (size_t) len);
		rd->ndel++;
		return;
	}
	len = snprintf(num, sizeof(num), "%s%lld", rd->nins ? "," : "", (long long) id);
	buf_append(&rd->ins, num, (size_t) len);
	len = snprintf(num, sizeof(num), "%s%d", rd->nins ? "," : "", (int32_t) (get_u64(d->to) >> 32));
	buf_append(&rd->k, num, (size_t) len);
	len = snprintf(num, sizeof(num), "%s%d", rd->nins ? "," : "", (int32_t) (uint32_t) get_u64(d->to));
	buf_append(&rd->v, num, (size_t) len);
	if (rd->nins)
		buf_append_byte(&rd->pad, ',');
	buf_append(&rd->pad, d->to + 8, d->tlen - 8);	/* pads are alphanumeric */
	rd->nins++;
}

/* Replays the rows that differ between two table roots as SQL on c. */
static uint64_t
replay_diff(PGconn *c, const PHash *from, const PHash *to)
{
	RowDiffs	rd;
	const char *vals[4];

	memset(&rd, 0, sizeof(rd));
	buf_append_byte(&rd.del, '{');
	buf_append_byte(&rd.ins, '{');
	buf_append_byte(&rd.k, '{');
	buf_append_byte(&rd.v, '{');
	buf_append_byte(&rd.pad, '{');
	CHECK_OK(prolly_diff(&repo.ctx, from, to, sql_diff_fn, &rd));
	buf_append(&rd.del, "}", 2);
	buf_append(&rd.ins, "}", 2);
	buf_append(&rd.k, "}", 2);
	buf_append(&rd.v, "}", 2);
	buf_append(&rd.pad, "}", 2);
	PQclear(qpc(c, "BEGIN", 0, NULL, NULL, NULL, 0));
	vals[0] = (const char *) rd.del.data;
	PQclear(qpc(c, "DELETE FROM t WHERE id = ANY($1::bigint[])", 1, vals, NULL, NULL, 0));
	vals[0] = (const char *) rd.ins.data;
	vals[1] = (const char *) rd.k.data;
	vals[2] = (const char *) rd.v.data;
	vals[3] = (const char *) rd.pad.data;
	PQclear(qpc(c, "INSERT INTO t SELECT * FROM unnest($1::bigint[], $2::int[], $3::int[], $4::text[]) "
				"ON CONFLICT (id) DO UPDATE SET k = excluded.k, v = excluded.v, pad = excluded.pad",
				4, vals, NULL, NULL, 0));
	PQclear(qpc(c, "COMMIT", 0, NULL, NULL, NULL, 0));
	buf_free(&rd.del);
	buf_free(&rd.ins);
	buf_free(&rd.k);
	buf_free(&rd.v);
	buf_free(&rd.pad);
	return rd.ndel + rd.nins;
}

static void
count_fn(void *arg, const ProllyDiff *d)
{
	(*(uint64_t *) arg)++;
}

static uint64_t
rows_between(const PHash *from_rv, const PHash *to_rv)
{
	uint64_t	n = 0;
	PHash		a = table_root(from_rv),
				b = table_root(to_rv);

	CHECK_OK(prolly_diff(&repo.ctx, &a, &b, count_fn, &n));
	return n;
}

/* Pages written since the working set's base: warm, since the base checkpointed. */
static uint64_t
dirty_since_base(void)
{
	PageSet		s = {0};
	char		lsn[32];
	uint64_t	n;

	flush_lsn(lsn);
	wal_pages(base_lsn, lsn, &s);
	n = s.n;
	free(s.v);
	return n;
}

/* Pages a restore to image img would write: changed since the base, or differing. */
static uint64_t
restore_pages(const PHash *img)
{
	ImgHeader	th,
				bh;
	PageSet		s = {0};
	char		lsn[32];
	uint64_t	n;

	CHECK_OK(img_load(&repo.ctx, img, &th));
	CHECK_OK(img_load(&repo.ctx, &base_img, &bh));
	flush_lsn(lsn);
	wal_pages(base_lsn, lsn, &s);
	CHECK_OK(img_diff(&repo.ctx, &bh, &th, img_diff_fn, &s));
	pageset_sort(&s);
	n = s.n;
	for (size_t i = 1; i < s.n; i++)
		if (s.v[i].oid == s.v[i - 1].oid && s.v[i].fork == s.v[i - 1].fork &&
			s.v[i].block == s.v[i - 1].block)
			n--;
	img_header_free(&th);
	img_header_free(&bh);
	free(s.v);
	return n;
}

static uint64_t checkout_sql(const PHash *target);

/*
 * Checks out target the way the planner prices cheapest: replay the rows,
 * or restore the target's image or its nearest imaged ancestor's and
 * replay the rows from there.  Returns rows replayed plus pages written.
 */
static uint64_t
checkout_planned(const PHash *target, uint64_t table_pages, char *how, size_t howlen)
{
	PHash		work = repo.br[repo.cur].working,
				kf[2];
	KfCandidate cand[2];
	size_t		n = 0;
	VcCommit	tc;
	uint64_t	rows = rows_between(&work, NULL == target ? &work : &work),
				done;
	KfChoice	ch;
	double		t;

	CHECK(vc_commit_load(&repo.ctx, target, &tc));
	rows = rows_between(&work, &tc.root);
	/* the target's own image, and the nearest imaged ancestor's */
	{
		PHash		h = *target;

		for (int hop = 0; hop < 64 && n < 2; hop++)
		{
			VcCommit	c;
			PHash		img;

			if (!vc_commit_load(&repo.ctx, &h, &c))
				break;
			if (vc_image_get(&repo, &h, &img))
			{
				memset(&cand[n], 0, sizeof(KfCandidate));
				cand[n].keyframe = h;
				cand[n].pages = restore_pages(&img);
				cand[n].after.rows = hop ? rows_between(&c.root, &tc.root) : 0;
				cand[n].after.pages = (uint64_t) ((double) cand[n].after.rows * density);
				/* rows replayed after a restore land on pages it just wrote */
				cand[n].after.warm_pages = cand[n].after.pages;
				kf[n++] = h;
				break;
			}
			if (c.nparents == 0)
				break;
			h = c.parents[0];
		}
	}
	{
		uint64_t	rp = (uint64_t) ((double) rows * density),
					warm = dirty_since_base();
		KfMove		m;

		m.rows = rows;
		m.pages = rp < table_pages ? rp : table_pages;
		m.warm_pages = warm < m.pages ? warm : m.pages;
		ch = planner->ops->choose(planner, &m, cand, n);
		t = now_ms();
		if (ch.path == KF_REPLAY)
		{
			done = checkout_sql(target);
			planner->ops->observe_replay(planner, &m, (now_ms() - t) * 1000);
			snprintf(how, howlen, "replay,rows=%llu,warm=%llu/%llu", (unsigned long long) rows,
					 (unsigned long long) m.warm_pages, (unsigned long long) m.pages);
		}
		else
		{
			const KfCandidate *k = &cand[ch.candidate];
			double		t2;

			done = checkout_images(&kf[ch.candidate]);
			t2 = now_ms();
			planner->ops->observe_restore(planner, k->pages, (t2 - t) * 1000);
			if (k->after.rows)
			{
				VcCommit	kc;
				PHash		from,
							to = table_root(&tc.root);

				CHECK(vc_commit_load(&repo.ctx, &kf[ch.candidate], &kc));
				from = table_root(&kc.root);
				done += replay_diff(db, &from, &to);
				qx("SELECT pg_replication_slot_advance('prolly', pg_current_wal_flush_lsn())");
				planner->ops->observe_replay(planner, &k->after, (now_ms() - t2) * 1000);
			}
			snprintf(how, howlen, "restore%s,pages=%llu,rows_after=%llu",
					 k->after.rows ? "+replay" : "", (unsigned long long) k->pages,
					 (unsigned long long) k->after.rows);
		}
	}
	snprintf(how + strlen(how), howlen - strlen(how),
			 ",est_ms=%.1f,replay_est_ms=%.1f,run_ms=%.1f%s", ch.est_us / 1000,
			 ch.replay_us / 1000, now_ms() - t, ch.explored ? ",explored" : "");
	return done;
}

/* Baseline: replays the prolly diff as SQL; returns rows changed. */
static uint64_t
checkout_sql(const PHash *target)
{
	VcCommit	c;
	PHash		from = table_root(&repo.br[repo.cur].working),
				to;
	uint64_t	n;

	CHECK(vc_commit_load(&repo.ctx, target, &c));
	to = table_root(&c.root);
	n = replay_diff(db, &from, &to);
	qx("SELECT pg_replication_slot_advance('prolly', pg_current_wal_flush_lsn())");
	/* the pages diverged from any image; base_img and base_lsn still describe them */
	return n;
}

/* -------------------------------------------------------------- verify */

/* Rows reached through each index must be exactly the rows the tree has. */
static void
verify_index_counts(const PHash *data)
{
	PGresult   *r;
	uint64_t	want = t_count(&repo.ctx, data);

	qx("SET enable_seqscan = off");
	qx("SET enable_bitmapscan = off");
	r = q("SELECT (SELECT count(*) FROM t WHERE id > -9223372036854775807), "
		  "(SELECT count(*) FROM t WHERE k >= 0)");
	qx("RESET enable_seqscan");
	qx("RESET enable_bitmapscan");
	CHECK(strtoull(PQgetvalue(r, 0, 0), NULL, 10) == want);
	CHECK(strtoull(PQgetvalue(r, 0, 1), NULL, 10) == want);
	PQclear(r);
}

static void
verify_full(const PHash *data)
{
	PGresult   *r;
	ProllyIter *it = t_iter(&repo.ctx, data, NULL, 0);
	char	   *line;
	int			len;

	r = q("COPY (SELECT id, k, v, pad FROM t ORDER BY id) TO STDOUT");
	CHECK(PQresultStatus(r) == PGRES_COPY_OUT);
	PQclear(r);
	while ((len = PQgetCopyData(db, &line, 0)) > 0)
	{
		const uint8_t *k,
				   *v;
		size_t		kl,
					vl;
		long long	id,
					kk,
					vv;
		char	   *p = line;

		CHECK(t_next(it, &k, &kl, &v, &vl));
		id = strtoll(p, &p, 10);
		kk = strtoll(p + 1, &p, 10);
		vv = strtoll(p + 1, &p, 10);
		p++;
		line[len - 1] = '\0';
		CHECK((uint64_t) id == (get_u64(k) ^ (1ULL << 63)));
		CHECK(kk == (int32_t) (get_u64(v) >> 32) && vv == (int32_t) (uint32_t) get_u64(v));
		CHECK(strlen(p) == vl - 8 && memcmp(p, v + 8, vl - 8) == 0);
		PQfreemem(line);
	}
	{
		const uint8_t *k,
				   *v;
		size_t		kl,
					vl;

		CHECK(!t_next(it, &k, &kl, &v, &vl));
	}
	prolly_iter_close(it);
	PQclear(PQgetResult(db));
	qx("SELECT bt_index_check('t_pkey', true), bt_index_check('t_k', true)");
	verify_index_counts(data);
}

typedef struct SampleIds
{
	Buf			arr;
	int			n;
} SampleIds;

static void
sample_fn(void *arg, const ProllyDiff *d)
{
	SampleIds  *s = arg;
	char		num[32];
	int			len;

	if (s->n >= 2000)
		return;
	len = snprintf(num, sizeof(num), "%s%lld", s->n ? "," : "",
				   (long long) (get_u64(d->key) ^ (1ULL << 63)));
	buf_append(&s->arr, num, (size_t) len);
	s->n++;
}

/* The rows that differ between from and to must read back as in to. */
static void
verify_changed(const PHash *from, const PHash *to)
{
	SampleIds	s = {BUF_INIT, 0};
	const char *vals[1];
	PGresult   *r;
	int			found = 0;

	buf_append_byte(&s.arr, '{');
	CHECK_OK(prolly_diff(&repo.ctx, from, to, sample_fn, &s));
	buf_append(&s.arr, "}", 2);
	vals[0] = (const char *) s.arr.data;
	r = qp("SELECT id, k, v, pad FROM t WHERE id = ANY($1::bigint[]) ORDER BY id", 1, vals,
		   NULL, NULL, 0);
	for (int i = 0; i < PQntuples(r); i++)
	{
		uint8_t		key[8];
		const uint8_t *v;
		size_t		vl;
		const char *pad = PQgetvalue(r, i, 3);

		row_key(key, strtoll(PQgetvalue(r, i, 0), NULL, 10));
		CHECK(t_get(&repo.ctx, to, key, 8, &v, &vl));
		CHECK(atoi(PQgetvalue(r, i, 1)) == (int32_t) (get_u64(v) >> 32));
		CHECK(atoi(PQgetvalue(r, i, 2)) == (int32_t) (uint32_t) get_u64(v));
		CHECK(strlen(pad) == vl - 8 && memcmp(pad, v + 8, vl - 8) == 0);
		found++;
	}
	PQclear(r);
	/* ids absent from the result must be absent from to */
	{
		const char *p = (const char *) s.arr.data + 1;
		int			present = 0;

		for (int i = 0; i < s.n; i++)
		{
			uint8_t		key[8];
			const uint8_t *v;
			size_t		vl;

			row_key(key, strtoll(p, (char **) &p, 10));
			p++;
			present += t_get(&repo.ctx, to, key, 8, &v, &vl);
		}
		CHECK(present == found);
	}
	buf_free(&s.arr);
}

/* ------------------------------------------------------------------ main */

static int
pick_in_window(void)
{
	for (int tries = 0; tries < 1000; tries++)
	{
		int			i = (int) rnd_below((uint64_t) ncommits);

		if (vc_locate(&repo, &commits[i].hash, NULL) == VC_LOC_WINDOW)
			return i;
	}
	return ncommits - 1;
}

static int
distance(const PHash *a, const PHash *b)
{
	VcCommit	ca,
				cb;

	CHECK(vc_commit_load(&repo.ctx, a, &ca) && vc_commit_load(&repo.ctx, b, &cb));
	return (int) ca.generation - (int) cb.generation;
}

/* Commits between a and b through their merge base: how far apart they are. */
static int
path_len(const PHash *a, const PHash *b)
{
	PHash		base;
	VcCommit	ca,
				cb,
				cbase;

	if (vc_merge_base_in(&repo, a, b, 0, &base) != 1 ||
		!vc_commit_load(&repo.ctx, &base, &cbase))
		return -1;				/* the base is past the cut */
	CHECK(vc_commit_load(&repo.ctx, a, &ca) && vc_commit_load(&repo.ctx, b, &cb));
	return (int) (ca.generation - cbase.generation + cb.generation - cbase.generation);
}

static void
switch_to(const PHash *target, bool by_sql, uint64_t db_pages)
{
	PHash		from = table_root(&repo.br[repo.cur].working);
	VcCommit	c;
	PHash		to;
	double		t0 = now_ms(),
				t1;
	uint64_t	work;
	int			dist = distance(&repo.br[repo.cur].head, target);
	int			path = path_len(&repo.br[repo.cur].head, target);

	char		how[256] = "";

	CHECK(vc_commit_load(&repo.ctx, target, &c));
	to = table_root(&c.root);
	if (planner && !by_sql)
		work = checkout_planned(target, db_pages, how, sizeof(how));
	else
		work = by_sql ? checkout_sql(target) : checkout_images(target);
	t1 = now_ms();
	CHECK(vc_reset_hard(&repo, target) == VC_OK);
	if (o.verify == 2)
		verify_full(&to);
	else if (o.verify == 1)
		verify_changed(&from, &to);
	printf("checkout,%s,%s,dist=%d,path=%d,%s=%llu,dirty=%llu,diff=%llu,ms=%.1f,db_pages=%llu,verify_ms=%.0f%s%s\n",
		   by_sql ? "sql" : how[0] ? "planned" : "image", hex(target), dist, path,
		   by_sql ? "rows" : how[0] ? "work" : "pages",
		   (unsigned long long) work, by_sql ? 0ULL : (unsigned long long) last_dirty,
		   by_sql ? 0ULL : (unsigned long long) last_diff, t1 - t0,
		   (unsigned long long) db_pages, now_ms() - t1, how[0] ? "," : "", how);
	fflush(stdout);
}

/* ------------------------------------------------------------- lifecycle */

typedef struct CkptStats
{
	uint64_t	count;
	uint64_t	rows;
	double		ms;
	double		max_ms;
} CkptStats;

static CkptStats bg_stats;

/*
 * Checkpoint: decode everything committed so far into the working root.
 * Caller holds repo_lock.  Returns rows decoded; *ms the time taken.
 */
static size_t
checkpoint_now(double *ms)
{
	char		lsn[32];
	double		t = now_ms();
	size_t		rows;

	flush_lsn_on(dec, lsn);
	rows = decode_into_prolly(lsn);
	snprintf(working_lsn, sizeof(working_lsn), "%s", lsn);
	*ms = now_ms() - t;
	return rows;
}

/* Background checkpoints, by time or by undecoded WAL, while writers run. */
static void *
bg_main(void *arg)
{
	double		last = now_ms();

	while (!bg_stop)
	{
		bool		due;

		usleep(20000);
		pthread_mutex_lock(&repo_lock);
		due = now_ms() - last >= o.bg_ms;
		if (!due && o.bg_bytes > 0)
		{
			const char *vals[1] = {working_lsn};
			PGresult   *r = qpc(dec, "SELECT pg_current_wal_lsn() - $1::pg_lsn", 1, vals,
								NULL, NULL, 0);

			due = strtoll(PQgetvalue(r, 0, 0), NULL, 10) >= o.bg_bytes;
			PQclear(r);
		}
		if (due)
		{
			double		ms;
			size_t		rows = checkpoint_now(&ms);

			bg_stats.count++;
			bg_stats.rows += rows;
			bg_stats.ms += ms;
			if (ms > bg_stats.max_ms)
				bg_stats.max_ms = ms;
			last = now_ms();
		}
		pthread_mutex_unlock(&repo_lock);
	}
	return NULL;
}

/* --------------------------------------------------------- stream decoder */

/*
 * One persistent logical replication stream, owned by its thread.  It
 * parses committed changes as they arrive, applies them to the working
 * root in batches (by row count or time: the background checkpoints), and
 * confirms its position so the slot advances.  A synchronous checkpoint
 * is a barrier: a logical message with a fresh token, emitted after the
 * work to include; when the stream reaches it, everything committed before
 * it is applied, and the waiter is woken.
 */
static PGconn *repl;
static pthread_mutex_t bar_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bar_cv = PTHREAD_COND_INITIALIZER;
static uint64_t bar_next,
			bar_seen;
static size_t bar_rows;			/* rows the last barrier applied */
static PHash bar_root;			/* the working root at the last barrier, held */
static volatile int64_t cw_done;	/* concurrent writer transactions committed */
static int64_t cw_upper;		/* ... as of the last barrier's return */
static bool bar_root_held;

static uint64_t
get_be64(const char *p)
{
	uint64_t	v = 0;

	for (int i = 0; i < 8; i++)
		v = v << 8 | (uint8_t) p[i];
	return v;
}

static void
put_be64(char *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--, v >>= 8)
		p[i] = (char) v;
}

static void
send_feedback(uint64_t lsn)
{
	char		buf[34];
	struct timespec ts;
	uint64_t	us;

	clock_gettime(CLOCK_REALTIME, &ts);
	us = (uint64_t) ts.tv_sec * 1000000 + (uint64_t) ts.tv_nsec / 1000 -
		UINT64_C(946684800) * 1000000;	/* since 2000-01-01 */
	buf[0] = 'r';
	put_be64(buf + 1, lsn);
	put_be64(buf + 9, lsn);
	put_be64(buf + 17, lsn);
	put_be64(buf + 25, us);
	buf[33] = 0;
	CHECK(PQputCopyData(repl, buf, sizeof(buf)) == 1);
	CHECK(PQflush(repl) == 0);
}

static void *
stream_main(void *arg)
{
	EditList	txn = {0},
				pend = {0};
	uint64_t	commit_lsn = 0,
				applied_lsn = 0;
	double		last_apply = now_ms(),
				last_fb = now_ms();
	Buf			line = BUF_INIT;

	while (!bg_stop)
	{
		char	   *msg;
		int			len = PQgetCopyData(repl, &msg, 1);
		bool		apply_now = false;
		uint64_t	token = 0;

		if (len == 0)
		{
			fd_set		rfds;
			struct timeval tv = {0, 20000};

			FD_ZERO(&rfds);
			FD_SET(PQsocket(repl), &rfds);
			select(PQsocket(repl) + 1, &rfds, NULL, NULL, &tv);
			CHECK(PQconsumeInput(repl) == 1);
		}
		else if (len < 0)
		{
			fprintf(stderr, "\nstream ended: %s\n", PQerrorMessage(repl));
			exit(1);
		}
		else
		{
			if (msg[0] == 'w' && len > 25)
			{
				uint64_t	start = get_be64(msg + 1);

				line.len = 0;
				buf_append(&line, msg + 25, (size_t) len - 25);
				buf_append_byte(&line, 0);
				if (strncmp((char *) line.data, "table ", 6) == 0)
					parse_change((char *) line.data, &txn);
				else if (strncmp((char *) line.data, "COMMIT", 6) == 0)
				{
					editlist_move(&pend, &txn);
					commit_lsn = start;
				}
				else if (strncmp((char *) line.data, "message:", 8) == 0 &&
						 strstr((char *) line.data, "prefix: prolly-ckpt"))
				{
					const char *c = strstr((char *) line.data, "content:");

					token = c ? strtoull(c + 8, NULL, 10) : 0;
					apply_now = true;
				}
			}
			else if (msg[0] == 'k' && len >= 18 && msg[17])
				send_feedback(applied_lsn);
			PQfreemem(msg);
		}

		if (apply_now || (size_t) pend.n >= (size_t) o.bg_rows ||
			(pend.n > 0 && now_ms() - last_apply >= o.bg_ms))
		{
			double		t = now_ms();
			size_t		rows;

			pthread_mutex_lock(&repo_lock);
			rows = editlist_apply(&pend);
			if (token)
			{
				/* hold the root as of the barrier: later applies replace it */
				if (bar_root_held)
					cs_unref_root(repo.ctx.cs, &bar_root);
				bar_root = vc_working(&repo);
				cs_ref_root(repo.ctx.cs, &bar_root);
				bar_root_held = true;
			}
			pthread_mutex_unlock(&repo_lock);
			applied_lsn = commit_lsn;
			last_apply = now_ms();
			if (token == 0 && rows > 0)
			{
				double		ms = now_ms() - t;

				bg_stats.count++;
				bg_stats.rows += rows;
				bg_stats.ms += ms;
				if (ms > bg_stats.max_ms)
					bg_stats.max_ms = ms;
			}
			if (token)
			{
				pthread_mutex_lock(&bar_mu);
				bar_seen = token;
				bar_rows = rows;
				pthread_cond_broadcast(&bar_cv);
				pthread_mutex_unlock(&bar_mu);
			}
		}
		if (now_ms() - last_fb >= 1000)
		{
			send_feedback(applied_lsn);
			last_fb = now_ms();
		}
	}
	buf_free(&line);
	return NULL;
}

/* Waits until everything committed so far is in the working root. */
static size_t
checkpoint(double *ms)
{
	double		t = now_ms();
	size_t		rows;

	if (o.sql_decode)
	{
		pthread_mutex_lock(&repo_lock);
		rows = checkpoint_now(ms);
		pthread_mutex_unlock(&repo_lock);
		return rows;
	}
	{
		uint64_t	token;
		char		tok_s[24];
		const char *vals[1] = {tok_s};

		pthread_mutex_lock(&bar_mu);
		token = ++bar_next;
		pthread_mutex_unlock(&bar_mu);
		snprintf(tok_s, sizeof(tok_s), "%llu", (unsigned long long) token);
		PQclear(qp("SELECT pg_logical_emit_message(false, 'prolly-ckpt', $1, true)", 1, vals,
				   NULL, NULL, 0));
		pthread_mutex_lock(&bar_mu);
		while (bar_seen < token)
			pthread_cond_wait(&bar_cv, &bar_mu);
		rows = bar_rows;
		pthread_mutex_unlock(&bar_mu);
	}
	*ms = now_ms() - t;
	return rows;
}

static void
start_stream(const char *from_lsn)
{
	char		conninfo[512];
	char		cmd[256];
	PGresult   *r;

	snprintf(conninfo, sizeof(conninfo), "%s replication=database", o.conninfo);
	repl = PQconnectdb(conninfo);
	if (PQstatus(repl) != CONNECTION_OK)
	{
		fprintf(stderr, "replication connect: %s", PQerrorMessage(repl));
		exit(1);
	}
	snprintf(cmd, sizeof(cmd),
			 "START_REPLICATION SLOT prolly LOGICAL %s (\"include-xids\" '0', "
			 "\"skip-empty-xacts\" '1', \"only-local\" '1')", from_lsn);
	r = PQexec(repl, cmd);
	if (PQresultStatus(r) != PGRES_COPY_BOTH)
	{
		fprintf(stderr, "START_REPLICATION: %s", PQerrorMessage(repl));
		exit(1);
	}
	PQclear(r);
}

static void
life_commit(int seq, const char *branch)
{
	double		ms,
				t0 = now_ms();
	size_t		tail;
	char		msg[64];
	PHash		c;
	uint32_t	seg;

	tail = checkpoint(&ms);
	if (o.concurrent)
	{
		/* let later transactions land in the working root before we commit */
		cw_upper = __atomic_load_n(&cw_done, __ATOMIC_SEQ_CST);
		usleep(150000);
	}
	pthread_mutex_lock(&repo_lock);
	snprintf(msg, sizeof(msg), "%s %d", branch, seq);
	/* exactly the state as of the barrier, whatever has landed since */
	c = o.sql_decode ? vc_commit(&repo, msg) : vc_commit_root(&repo, &bar_root, msg);
	CHECK(vc_sever(&repo, &(VcWindow) {.depth = (uint32_t) o.window}, &seg) == VC_OK);
	if (seg != VC_NO_SEGMENT && archive)
		CHECK(vc_segment_export(&repo, seg, &(VcSource) {"archive", archive}) == VC_OK);
	else if (seg != VC_NO_SEGMENT)
		CHECK(vc_segment_discard(&repo, seg) == VC_OK);
	/* with the history store in Postgres, the commit is durable once flushed */
	if (store_pg)
	{
		PHash		st = vc_repo_save(&repo);

		pgstore_set_state(cs, &st);
		CHECK_OK(pgstore_flush(cs));
	}
	CHECK(ncommits < MAX_COMMITS);
	commits[ncommits].hash = c;
	snprintf(commits[ncommits].branch, sizeof(commits[ncommits].branch), "%s", branch);
	commits[ncommits].seq = seq;
	ncommits++;
	printf("commit,%s,%d,%s,tail_rows=%zu,ckpt_ms=%.1f,commit_ms=%.1f,store_live_mb=%.0f\n",
		   branch, seq, hex(&c), tail, ms, now_ms() - t0, cs->ops->bytes(cs) / 1048576.0);
	pthread_mutex_unlock(&repo_lock);
	fflush(stdout);
}

static void
count_diff_fn(void *arg, const ProllyDiff *d)
{
	(*(uint64_t *) arg)++;
}

/* diff WORKING X: checkpoint, then the prolly tree alone. */
static void
life_diff(const PHash *target)
{
	double		ms,
				t0 = now_ms(),
				t1;
	size_t		tail;
	uint64_t	d = 0;
	VcCommit	c;
	PHash		from,
				to;
	int			path;

	tail = checkpoint(&ms);
	pthread_mutex_lock(&repo_lock);
	t1 = now_ms();
	CHECK(vc_commit_load(&repo.ctx, target, &c));
	from = table_root(&repo.br[repo.cur].working);
	to = table_root(&c.root);
	CHECK_OK(prolly_diff(&repo.ctx, &from, &to, count_diff_fn, &d));
	path = path_len(&repo.br[repo.cur].head, target);
	printf("diff,%s,path=%d,tail_rows=%zu,rows=%llu,ckpt_ms=%.1f,diff_ms=%.1f,total_ms=%.1f\n",
		   hex(target), path, tail, (unsigned long long) d, ms, now_ms() - t1, now_ms() - t0);
	pthread_mutex_unlock(&repo_lock);
	fflush(stdout);
}

/*
 * Moves the working set: to commit target on the current branch (reset),
 * or to another branch's working set (switch, when branch is set).
 */
static void
life_checkout(const PHash *target, const char *branch)
{
	double		ms,
				t0 = now_ms(),
				t1;
	size_t		tail;
	uint64_t	rows;
	PHash		from,
				to;
	int			path;

	tail = checkpoint(&ms);
	pthread_mutex_lock(&repo_lock);
	t1 = now_ms();
	from = table_root(&repo.br[repo.cur].working);
	if (branch)
	{
		VcBranch   *b = vc_branch_find(&repo, branch);

		path = path_len(&repo.br[repo.cur].head, &b->head);
		CHECK(vc_checkout(&repo, branch) == VC_OK);
		to = table_root(&repo.br[repo.cur].working);
	}
	else
	{
		VcCommit	c;

		path = path_len(&repo.br[repo.cur].head, target);
		CHECK(vc_commit_load(&repo.ctx, target, &c));
		to = table_root(&c.root);
	}
	/* replay before a reset: the reset lets go of the tree we diff from */
	rows = replay_diff(rep, &from, &to);
	if (!branch)
		CHECK(vc_reset_hard(&repo, target) == VC_OK);
	printf("checkout,replay,%s,path=%d,tail_rows=%zu,rows=%llu,ckpt_ms=%.1f,replay_ms=%.1f,"
		   "total_ms=%.1f\n", branch ? branch : hex(target), path, tail,
		   (unsigned long long) rows, ms, now_ms() - t1, now_ms() - t0);
	if (o.verify == 2)
		verify_full(&to);
	else if (o.verify == 1)
		verify_changed(&from, &to);
	pthread_mutex_unlock(&repo_lock);
	fflush(stdout);
}

static PGconn *
connect_or_die(void)
{
	PGconn	   *c = PQconnectdb(o.conninfo);

	if (PQstatus(c) != CONNECTION_OK)
	{
		fprintf(stderr, "connect: %s", PQerrorMessage(c));
		exit(1);
	}
	return c;
}

/* ----------------------------------------------------- concurrent writer */

#define CW_BASE INT64_C(1000000000000)
#define CW_ROWS 50				/* rows per writer transaction */

static volatile bool cw_stop;

/* Commits transaction after transaction of new rows, ids in order. */
static void *
writer_main(void *arg)
{
	PGconn	   *w = connect_or_die();
	char		sql[256];

	for (int64_t txn = 0; !cw_stop; txn++)
	{
		int64_t		lo = CW_BASE + txn * CW_ROWS;

		snprintf(sql, sizeof(sql),
				 "INSERT INTO t SELECT g, 1, 7, 'w' FROM generate_series(%lld::bigint, %lld::bigint) g",
				 (long long) lo, (long long) (lo + CW_ROWS - 1));
		PQclear(qpc(w, sql, 0, NULL, NULL, NULL, 0));
		__atomic_store_n(&cw_done, txn + 1, __ATOMIC_SEQ_CST);
	}
	PQfinish(w);
	return NULL;
}

/*
 * Each commit, taken while the writer runs, must hold exactly a prefix of
 * its transactions (no transaction split, none skipped), and at least all
 * that had committed before the commit began.
 */
static void
run_concurrent(void)
{
	pthread_t	wt;
	int64_t		lower[64],
				upper[64];
	int			n = 0;

	pthread_create(&wt, NULL, writer_main, NULL);
	for (; n < 40; n++)
	{
		usleep((useconds_t) (20000 + rnd_below(80000)));
		lower[n] = __atomic_load_n(&cw_done, __ATOMIC_SEQ_CST);
		life_commit(n + 1, "main");
		upper[n] = cw_upper;
	}
	cw_stop = true;
	pthread_join(wt, NULL);
	{
		double		ms;

		checkpoint(&ms);		/* the working root catches up with the writer */
	}
	pthread_mutex_lock(&repo_lock);
	for (int i = 0; i < n; i++)
	{
		const PHash *c = &commits[ncommits - n + i].hash;
		VcCommit	cm;
		PHash		data;
		uint8_t		start[8];
		ProllyIter *it;
		const uint8_t *k,
				   *v;
		size_t		kl,
					vl;
		int64_t		expect = CW_BASE,
					rows = 0;

		CHECK(vc_commit_load(&repo.ctx, c, &cm));
		data = table_root(&cm.root);
		row_key(start, CW_BASE);
		it = t_iter(&repo.ctx, &data, start, 8);
		while (t_next(it, &k, &kl, &v, &vl))
		{
			int64_t		id = (int64_t) (get_u64(k) ^ (1ULL << 63));

			CHECK(id == expect);	/* contiguous from the first: a prefix */
			expect++;
			rows++;
		}
		prolly_iter_close(it);
		CHECK(rows % CW_ROWS == 0);	/* whole transactions only */
		CHECK(rows / CW_ROWS >= lower[i]);	/* nothing committed before is missing */
		/* nothing from after the barrier: at most the one then in flight */
		CHECK(rows / CW_ROWS <= upper[i] + 1);
		printf("concurrent,commit=%d,writer_txns=%lld,committed_before=%lld,at_barrier=%lld\n",
			   i + 1, (long long) (rows / CW_ROWS), (long long) lower[i], (long long) upper[i]);
	}
	pthread_mutex_unlock(&repo_lock);
	printf("# concurrent: %d commits each held a whole prefix of the writer's %lld transactions\n",
		   n, (long long) cw_done);
}

/* ---------------------------------------------------------- outside window */

/* Brings an exported commit back from the archive; returns ms, *chunks copied. */
static double
attach_if_outside(const PHash *x, uint64_t *chunks, uint64_t *bytes)
{
	uint32_t	seg;
	uint64_t	n0 = cs->stats.new_chunks,
				b0 = cs->stats.new_bytes;
	double		t = now_ms();

	*chunks = *bytes = 0;
	if (vc_locate(&repo, x, &seg) != VC_LOC_EXPORTED)
		return 0;
	CHECK(vc_segment_attach(&repo, seg) == VC_OK);
	*chunks = cs->stats.new_chunks - n0;
	*bytes = cs->stats.new_bytes - b0;
	return now_ms() - t;
}

/* The whole table rebuilt from the tree at data, on the replay connection. */
static double
rebuild_from(const PHash *data, uint64_t *rows)
{
	ProllyIter *it = t_iter(&repo.ctx, data, NULL, 0);
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	Buf			b = BUF_INIT;
	double		t = now_ms();
	PGresult   *r;

	*rows = 0;
	PQclear(qpc(rep, "BEGIN", 0, NULL, NULL, NULL, 0));
	PQclear(qpc(rep, "TRUNCATE t", 0, NULL, NULL, NULL, 0));
	r = PQexec(rep, "COPY t FROM STDIN");
	CHECK(PQresultStatus(r) == PGRES_COPY_IN);
	PQclear(r);
	while (t_next(it, &k, &kl, &v, &vl))
	{
		char		head[64];
		int			n = snprintf(head, sizeof(head), "%lld\t%d\t%d\t",
								 (long long) (get_u64(k) ^ (1ULL << 63)),
								 (int32_t) (get_u64(v) >> 32), (int32_t) (uint32_t) get_u64(v));

		buf_append(&b, head, (size_t) n);
		buf_append(&b, v + 8, vl - 8);
		buf_append_byte(&b, '\n');
		if (b.len > (1 << 20))
		{
			CHECK(PQputCopyData(rep, (const char *) b.data, (int) b.len) == 1);
			b.len = 0;
		}
		(*rows)++;
	}
	prolly_iter_close(it);
	if (b.len)
		CHECK(PQputCopyData(rep, (const char *) b.data, (int) b.len) == 1);
	CHECK(PQputCopyEnd(rep, NULL) == 1);
	r = PQgetResult(rep);
	CHECK(PQresultStatus(r) == PGRES_COMMAND_OK);
	PQclear(r);
	PQclear(PQgetResult(rep));
	PQclear(qpc(rep, "COMMIT", 0, NULL, NULL, NULL, 0));
	buf_free(&b);
	return now_ms() - t;
}

/*
 * diff and checkout against commits at each distance behind the head:
 * inside the window they are local; outside they come back from the
 * archive first.  Then one full rebuild, for the case nothing else fits.
 */
static void
run_outside(void)
{
	char	   *list = px_memdup(o.outside, strlen(o.outside) + 1);
	PHash		head = repo.br[repo.cur].head;
	int			last_main = ncommits - 1;
	bool		rebuilt = false;

	for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
	{
		int			dist = atoi(tok);
		PHash		x;
		VcCommit	c;
		PHash		from,
					to;
		uint64_t	chunks,
					bytes,
					d = 0,
					rows;
		double		ms,
					attach_ms,
					t;
		const char *where;

		if (dist > last_main)
			continue;
		x = commits[last_main - dist].hash;
		checkpoint(&ms);
		pthread_mutex_lock(&repo_lock);
		where = vc_locate(&repo, &x, NULL) == VC_LOC_WINDOW ? "window" : "outside";

		/* diff WORKING X */
		attach_ms = attach_if_outside(&x, &chunks, &bytes);
		t = now_ms();
		CHECK(vc_commit_load(&repo.ctx, &x, &c));
		from = table_root(&repo.br[repo.cur].working);
		to = table_root(&c.root);
		CHECK_OK(prolly_diff(&repo.ctx, &from, &to, count_diff_fn, &d));
		printf("outside,diff,dist=%d,%s,attach_ms=%.0f,attach_chunks=%llu,attach_mb=%.1f,"
			   "diff_ms=%.0f,rows=%llu,total_ms=%.0f\n", dist, where, attach_ms,
			   (unsigned long long) chunks, bytes / 1048576.0, now_ms() - t,
			   (unsigned long long) d, attach_ms + now_ms() - t);

		/* checkout X by replay, then back to the head (not measured) */
		t = now_ms();
		rows = replay_diff(rep, &from, &to);
		CHECK(vc_reset_hard(&repo, &x) == VC_OK);
		printf("outside,checkout,dist=%d,%s,attach_ms=%.0f,replay_ms=%.0f,rows=%llu,total_ms=%.0f\n",
			   dist, where, attach_ms, now_ms() - t, (unsigned long long) rows,
			   attach_ms + now_ms() - t);
		verify_changed(&from, &to);

		if (!rebuilt && strcmp(where, "outside") == 0)
		{
			uint64_t	n;
			double		rms = rebuild_from(&to, &n);

			printf("outside,rebuild,dist=%d,rows=%llu,ms=%.0f\n", dist, (unsigned long long) n,
				   rms);
			verify_changed(&from, &to);
			rebuilt = true;
		}
		replay_diff(rep, &to, &from);
		CHECK(vc_reset_hard(&repo, &head) == VC_OK);
		pthread_mutex_unlock(&repo_lock);
		fflush(stdout);
	}
	free(list);
}

static void
run_lifecycle(void)
{
	pthread_t	bg;
	PHash		c0;
	char		lsn[32];

	dec = connect_or_die();
	rep = connect_or_die();
	PQclear(qpc(rep, "SELECT pg_replication_origin_create('prolly_replay') "
				"WHERE NOT EXISTS (SELECT 1 FROM pg_replication_origin WHERE roname = 'prolly_replay')",
				0, NULL, NULL, NULL, 0));
	PQclear(qpc(rep, "SELECT pg_replication_origin_session_setup('prolly_replay')",
				0, NULL, NULL, NULL, 0));

	c0 = vc_commit(&repo, "load");
	commits[ncommits++] = (CommitInfo) {c0, "main", 0};
	flush_lsn(lsn);
	qx("SELECT pg_replication_slot_advance('prolly', pg_current_wal_flush_lsn())");
	snprintf(working_lsn, sizeof(working_lsn), "%s", lsn);
	CHECK(vc_window_track(&repo, (uint32_t) o.window) == VC_OK);
	if (o.sql_decode)
		pthread_create(&bg, NULL, bg_main, NULL);
	else
	{
		start_stream(lsn);
		pthread_create(&bg, NULL, stream_main, NULL);
	}

	if (o.concurrent)
	{
		run_concurrent();
		goto finish;
	}
	for (int i = 1; i <= o.commits; i++)
	{
		workload(i);
		life_commit(i, "main");
	}
	if (o.outside)
	{
		run_outside();
		goto finish;
	}
	for (int b = 0; b < o.branches; b++)
	{
		char		name[32];
		int			from;

		pthread_mutex_lock(&repo_lock);
		from = pick_in_window();
		snprintf(name, sizeof(name), "b%d", b);
		CHECK(vc_branch_create(&repo, name, &commits[from].hash) == VC_OK);
		pthread_mutex_unlock(&repo_lock);
		life_checkout(NULL, name);
		for (int i = 1; i <= o.branch_commits; i++)
		{
			workload(1000 * (b + 1) + i);
			life_commit(i, name);
		}
	}
	/* diffs against commits in the window, with a fresh tail each time */
	for (int k = 0; k < o.diffs; k++)
	{
		int			t;

		workload(5000 + k);
		pthread_mutex_lock(&repo_lock);
		t = pick_in_window();
		pthread_mutex_unlock(&repo_lock);
		life_diff(&commits[t].hash);
	}
	/* switches between branch heads, then resets to commits in the window */
	for (int k = 0; k < o.checkouts && o.branches > 0; k++)
	{
		char		name[32];

		snprintf(name, sizeof(name), "b%d", (int) rnd_below((uint64_t) o.branches));
		life_checkout(NULL, name);
	}
	for (int k = 0; k < o.checkouts; k++)
	{
		int			t;

		pthread_mutex_lock(&repo_lock);
		t = pick_in_window();
		pthread_mutex_unlock(&repo_lock);
		life_checkout(&commits[t].hash, NULL);
	}
finish:
	bg_stop = true;
	pthread_join(bg, NULL);
	if (bar_root_held)
		cs_unref_root(repo.ctx.cs, &bar_root);
	printf("# decoding: %s\n", o.sql_decode ? "SQL function per checkpoint" : "persistent stream");
	printf("# background checkpoints: %llu, %llu rows, mean %.1f ms, max %.1f ms\n",
		   (unsigned long long) bg_stats.count, (unsigned long long) bg_stats.rows,
		   bg_stats.count ? bg_stats.ms / (double) bg_stats.count : 0, bg_stats.max_ms);
	if (o.final_verify)
	{
		PHash		td = table_root(&repo.br[repo.cur].working);

		verify_full(&td);
	}
}

static uint64_t
db_pages(void)
{
	uint64_t	n = 0;

	for (int i = 0; i < nrels; i++)
		n += rel_nblocks(&rels[i], FORK_MAIN);
	return n;
}

static void
usage(void)
{
	fprintf(stderr,
			"winbench [--rows N] [--pad B] [--commits N] [--changes N] [--txns N]\n"
			"         [--window W] [--checkouts N] [--branches N] [--branch-commits N]\n"
			"         [--verify 0|1|2] [--no-sql] [--reuse] [--no-final-verify] [--seed N]\n"
			"         [--lifecycle [--bg-ms MS] [--bg-rows N] [--diffs N]\n"
			"                      [--sql-decode [--bg-bytes B]]]\n"
			"         [--planned REPLAY_FIXED,ROW,COLD_PAGE,WARM_PAGE,RESTORE_FIXED,PAGE,CAPTURE (us)]\n"
			"         [--conn STR] --pgdata DIR [--store FILE]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	static struct option opts[] = {
		{"rows", 1, 0, 'r'}, {"pad", 1, 0, 'p'}, {"commits", 1, 0, 'c'},
		{"changes", 1, 0, 'n'}, {"txns", 1, 0, 'x'}, {"window", 1, 0, 'w'},
		{"checkouts", 1, 0, 'k'}, {"branches", 1, 0, 'b'}, {"branch-commits", 1, 0, 'B'},
		{"verify", 1, 0, 'v'}, {"no-sql", 0, 0, 'S'}, {"reuse", 0, 0, 'R'},
		{"seed", 1, 0, 's'}, {"conn", 1, 0, 'C'}, {"pgdata", 1, 0, 'D'},
		{"store", 1, 0, 'F'}, {"no-final-verify", 0, 0, 'N'}, {"lifecycle", 0, 0, 'L'},
		{"store-pg", 0, 0, 'G'},
		{"bg-ms", 1, 0, 'T'}, {"bg-bytes", 1, 0, 'Y'}, {"diffs", 1, 0, 'f'},
		{"sql-decode", 0, 0, 'Q'}, {"bg-rows", 1, 0, 'W'}, {"concurrent", 0, 0, 'K'},
		{"outside", 1, 0, 'O'}, {"planned", 1, 0, 'P'}, {0, 0, 0, 0}
	};
	int			ch;
	PHash		c0,
				img0;
	uint64_t	pages0;
	double		t0;

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'r': o.rows = atoll(optarg); break;
			case 'p': o.pad = atoi(optarg); break;
			case 'c': o.commits = atoi(optarg); break;
			case 'n': o.changes = atoi(optarg); break;
			case 'x': o.txns = atoi(optarg); break;
			case 'w': o.window = atoi(optarg); break;
			case 'k': o.checkouts = atoi(optarg); break;
			case 'b': o.branches = atoi(optarg); break;
			case 'B': o.branch_commits = atoi(optarg); break;
			case 'v': o.verify = atoi(optarg); break;
			case 'S': o.sql_baseline = false; break;
			case 'R': o.reuse = true; break;
			case 's': o.seed = (uint64_t) atoll(optarg); break;
			case 'C': o.conninfo = optarg; break;
			case 'D': o.pgdata = optarg; break;
			case 'F': o.store = optarg; break;
			case 'N': o.final_verify = false; break;
			case 'L': o.lifecycle = true; break;
			case 'G': store_pg = true; break;
			case 'T': o.bg_ms = atoi(optarg); break;
			case 'Y': o.bg_bytes = atoll(optarg); break;
			case 'f': o.diffs = atoi(optarg); break;
			case 'Q': o.sql_decode = true; break;
			case 'W': o.bg_rows = atoi(optarg); break;
			case 'K': o.concurrent = true; break;
			case 'O': o.outside = optarg; break;
			case 'P': o.planned = optarg; break;
			default: usage();
		}
	if (o.pgdata == NULL || o.txns < 1 || o.window < 1 || o.pad < 20)
		usage();
	if (o.planned)
	{
		KfCosts		k;

		KfCalibration cal = {.forget = 0.99, .prior_weight = 1, .explore_every = 50};

		memset(&k, 0, sizeof(k));
		if (sscanf(o.planned, "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &k.replay_fixed_us, &k.row_us,
				   &k.replay_page_us, &k.replay_warm_page_us, &k.restore_fixed_us, &k.page_us,
				   &k.capture_page_us) != 7)
			usage();
		planner = kf_calibrating_planner(&k, &cal);
	}
	rng_seed(o.seed);
	db = PQconnectdb(o.conninfo);
	if (PQstatus(db) != CONNECTION_OK)
	{
		fprintf(stderr, "connect: %s", PQerrorMessage(db));
		return 1;
	}
	{
		PGresult   *r = q("SELECT current_setting('server_version_num')::int / 10000, "
						  "current_setting('block_size')::int, "
						  "(SELECT catalog_version_no FROM pg_control_system()), "
						  "(SELECT system_identifier FROM pg_control_system())");

		fmt.pg_major = (uint32_t) atoi(PQgetvalue(r, 0, 0));
		fmt.page_size = (uint32_t) atoi(PQgetvalue(r, 0, 1));
		fmt.catversion = (uint32_t) atoi(PQgetvalue(r, 0, 2));
		fmt.system_id = strtoull(PQgetvalue(r, 0, 3), NULL, 10);
		PQclear(r);
		CHECK(fmt.page_size == BLCK);
	}

	if (store_pg)
	{
		char		err[512];
		const char *ci = getenv("PROLLY_PG");

		cs = pgstore_open(ci ? ci : "host=/tmp port=5499 user=postgres dbname=prolly_pgstore",
						  "winbench", true, false, err, sizeof(err));
		if (cs == NULL)
		{
			fprintf(stderr, "winbench: %s\n", err);
			return 1;
		}
	}
	else
		cs = filestore_create(o.store);
	if (o.outside)
	{
		char		apath[512];

		snprintf(apath, sizeof(apath), "%s.archive", o.store);
		archive = filestore_create(apath);
	}
	vc_repo_init(&repo, cs, &prolly_default_policy);
	setup_db();
	load_prolly();
	if (o.lifecycle)
	{
		run_lifecycle();
		printf("# done: %d commits, store live %.0f MB, db %llu pages (%.0f MB)\n", ncommits,
			   cs->ops->bytes(cs) / 1048576.0, (unsigned long long) db_pages(),
			   db_pages() * 8192.0 / 1048576.0);
		vc_repo_free(&repo);
		cs_destroy(cs);
		if (archive)
			cs_destroy(archive);
		PQfinish(rep);
		PQfinish(dec);
		if (repl)
			PQfinish(repl);
		PQfinish(db);
		return 0;
	}
	c0 = vc_commit(&repo, "load");
	t0 = now_ms();
	img0 = capture_full(&c0, &pages0);
	CHECK(vc_image_set(&repo, &c0, &img0) == VC_OK);
	qx("SELECT pg_replication_slot_advance('prolly', pg_current_wal_flush_lsn())");
	set_base_point(&img0);
	commits[ncommits++] = (CommitInfo) {c0, "main", 0};
	printf("# initial image: %llu pages in %.1fs, store %.0f MB\n", (unsigned long long) pages0,
		   (now_ms() - t0) / 1000, cs->ops->bytes(cs) / 1048576.0);

	for (int i = 1; i <= o.commits; i++)
	{
		workload(i);
		dolt_commit(i, "main");
	}
	/* branches off commits inside the window */
	for (int b = 0; b < o.branches; b++)
	{
		char		name[32];
		int			from = pick_in_window();

		snprintf(name, sizeof(name), "b%d", b);
		switch_to(&commits[from].hash, false, db_pages());
		CHECK(vc_branch_create(&repo, name, &commits[from].hash) == VC_OK);
		CHECK(vc_checkout(&repo, name) == VC_OK);
		for (int i = 1; i <= o.branch_commits; i++)
		{
			workload(1000 * (b + 1) + i);
			dolt_commit(i, name);
		}
	}
	/* checkouts to anything in the window, across branches */
	{
		uint64_t	dbp = db_pages();

		for (int k = 0; k < o.checkouts; k++)
			switch_to(&commits[pick_in_window()].hash, false, dbp);
	}

	/* switches between branch heads */
	if (o.branches > 0)
	{
		uint64_t	dbp = db_pages();

		for (int k = 0; k < o.checkouts; k++)
		{
			char		name[32];
			VcBranch   *b;

			snprintf(name, sizeof(name), "b%d", (int) rnd_below((uint64_t) o.branches));
			b = vc_branch_find(&repo, name);
			printf("# head switch to %s\n", name);
			switch_to(&b->head, false, dbp);
		}
	}

	/*
	 * The SQL baseline last: replaying rows leaves the pages diverged from
	 * every image, which would inflate image checkouts that came after.
	 */
	if (o.sql_baseline)
	{
		uint64_t	dbp = db_pages();

		for (int k = 0; k < o.checkouts; k++)
			switch_to(&commits[pick_in_window()].hash, true, dbp);
	}

	/* one full verification at the end */
	if (o.final_verify)
	{
		VcCommit	c;

		PHash		td;

		CHECK(vc_commit_load(&repo.ctx, &repo.br[repo.cur].head, &c));
		td = table_root(&c.root);
		verify_full(&td);
	}
	printf("# done: %d commits, store live %.0f MB, file %.0f MB, db %llu pages (%.0f MB)\n",
		   ncommits, cs->ops->bytes(cs) / 1048576.0, filestore_file_bytes(cs) / 1048576.0,
		   (unsigned long long) db_pages(), db_pages() * 8192.0 / 1048576.0);
	vc_repo_free(&repo);
	cs_destroy(cs);
	PQfinish(db);
	return 0;
}
