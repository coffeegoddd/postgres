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
 * Output: one CSV line per commit and per checkout, then a summary.
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../test/test_util.h"
#include "../image.h"

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
	uint64_t	seed;
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
	0, 5, 1, true, false, true, 1
};

static PGconn *db;
static uint32_t dboid;
static Rel rels[MAX_RELS];
static int nrels;
static ChunkStore *cs;
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

/* ----------------------------------------------------------------- utils */

static double
now_ms(void)
{
	return now_sec() * 1000.0;
}

static PGresult *
qp(const char *sql, int n, const char *const *vals, const int *lens,
   const int *fmts, int resfmt)
{
	PGresult   *r = PQexecParams(db, sql, n, NULL, vals, lens, fmts, resfmt);
	ExecStatusType st = PQresultStatus(r);

	if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK && st != PGRES_COPY_OUT)
	{
		fprintf(stderr, "\nquery failed: %s\n%s\n", sql, PQerrorMessage(db));
		exit(1);
	}
	return r;
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
flush_lsn(char *out)
{
	PGresult   *r = q("SELECT pg_logical_emit_message(false, 'prolly', '', true), "
					  "pg_current_wal_flush_lsn()");

	snprintf(out, 32, "%s", PQgetvalue(r, 0, 1));
	PQclear(r);
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
	PGresult   *r = qp("SELECT data FROM pg_logical_slot_get_changes('prolly', $1::pg_lsn, NULL, "
					   "'include-xids', '0', 'skip-empty-xacts', '1')", 1, vals, NULL, NULL, 0);
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
	img = capture_incremental(&c, lsn, &pages);
	CHECK(vc_image_set(&repo, &c, &img) == VC_OK);
	t2 = now_ms();
	set_base_point(&img);
	t3 = now_ms();

	/* the window: what falls out is discarded, images with it */
	CHECK(vc_sever(&repo, &(VcWindow) {(uint32_t) o.window, NULL, 0}, &seg) == VC_OK);
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

/* Baseline: replays the prolly diff as SQL; returns rows changed. */
static uint64_t
checkout_sql(const PHash *target)
{
	VcCommit	c;
	PHash		from = table_root(&repo.br[repo.cur].working),
				to;
	RowDiffs	rd;
	const char *vals[4];

	memset(&rd, 0, sizeof(rd));
	CHECK(vc_commit_load(&repo.ctx, target, &c));
	to = table_root(&c.root);
	buf_append_byte(&rd.del, '{');
	buf_append_byte(&rd.ins, '{');
	buf_append_byte(&rd.k, '{');
	buf_append_byte(&rd.v, '{');
	buf_append_byte(&rd.pad, '{');
	CHECK_OK(prolly_diff(&repo.ctx, &from, &to, sql_diff_fn, &rd));
	buf_append(&rd.del, "}", 2);
	buf_append(&rd.ins, "}", 2);
	buf_append(&rd.k, "}", 2);
	buf_append(&rd.v, "}", 2);
	buf_append(&rd.pad, "}", 2);
	qx("BEGIN");
	vals[0] = (const char *) rd.del.data;
	PQclear(qp("DELETE FROM t WHERE id = ANY($1::bigint[])", 1, vals, NULL, NULL, 0));
	vals[0] = (const char *) rd.ins.data;
	vals[1] = (const char *) rd.k.data;
	vals[2] = (const char *) rd.v.data;
	vals[3] = (const char *) rd.pad.data;
	PQclear(qp("INSERT INTO t SELECT * FROM unnest($1::bigint[], $2::int[], $3::int[], $4::text[]) "
			   "ON CONFLICT (id) DO UPDATE SET k = excluded.k, v = excluded.v, pad = excluded.pad",
			   4, vals, NULL, NULL, 0));
	qx("COMMIT");
	qx("SELECT pg_replication_slot_advance('prolly', pg_current_wal_flush_lsn())");
	/* the pages diverged from any image; base_img and base_lsn still describe them */
	buf_free(&rd.del);
	buf_free(&rd.ins);
	buf_free(&rd.k);
	buf_free(&rd.v);
	buf_free(&rd.pad);
	return rd.ndel + rd.nins;
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

	CHECK(vc_commit_load(&repo.ctx, target, &c));
	to = table_root(&c.root);
	work = by_sql ? checkout_sql(target) : checkout_images(target);
	t1 = now_ms();
	CHECK(vc_reset_hard(&repo, target) == VC_OK);
	if (o.verify == 2)
		verify_full(&to);
	else if (o.verify == 1)
		verify_changed(&from, &to);
	printf("checkout,%s,%s,dist=%d,path=%d,%s=%llu,dirty=%llu,diff=%llu,ms=%.1f,db_pages=%llu,verify_ms=%.0f\n",
		   by_sql ? "sql" : "image", hex(target), dist, path, by_sql ? "rows" : "pages",
		   (unsigned long long) work, by_sql ? 0ULL : (unsigned long long) last_dirty,
		   by_sql ? 0ULL : (unsigned long long) last_diff, t1 - t0,
		   (unsigned long long) db_pages, now_ms() - t1);
	fflush(stdout);
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
		{"store", 1, 0, 'F'}, {"no-final-verify", 0, 0, 'N'}, {0, 0, 0, 0}
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
			default: usage();
		}
	if (o.pgdata == NULL || o.txns < 1 || o.window < 1 || o.pad < 20)
		usage();
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

	cs = filestore_create(o.store);
	vc_repo_init(&repo, cs, &prolly_default_policy);
	setup_db();
	load_prolly();
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
