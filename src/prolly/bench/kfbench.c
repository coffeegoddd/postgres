/*
 * kfbench.c
 *	  Where restoring pages beats replaying rows: the keyframe crossover,
 *	  against a real server.
 *
 * A table t(id bigint primary key, k int, v int, pad text) with an index
 * on k.  For each change shape (n rows, spread at random or in one id
 * range, or every row), the table is moved from its base state S0 to a
 * changed state S1 by SQL, and then back from S1 to S0, timed, by:
 *	replay		the rows that differ, upserted in one statement (the way
 *				a checkout replays a prolly diff)
 *	pages-sql	the pages that differ, sent as bytea over libpq and logged
 *				one WAL record per page (prolly_put_pages)
 *	pages-file	the same pages read by the server from a file it can see,
 *				as a restore reading its own chunk store would
 *				(prolly_put_pages_file), one record per page
 *	pages-batch	the same, XLR_MAX_BLOCK_ID pages per WAL record
 * under each wal_compression in --compressions (replay and pages-batch
 * under every one; the other page paths, which differ only in transport,
 * under the first).  A checkpoint
 * precedes every timed move, so both kinds pay full-page writes the way a
 * checkout after one would; with --warm it does not, and replay finds its
 * pages already written since the last checkpoint (restore logs whole
 * pages either way), which is replay's best case.  Every move is checked: rows by checksum
 * against S0, and both indexes with amcheck.
 *
 * Pages that differ come from comparing the relation files to snapshots
 * taken after a checkpoint, outside the timing; in the engine they come
 * from an image diff and the WAL, in O(pages changed).
 *
 * Prints, per shape and method: rows, pages, the median ms of --reps
 * moves, and WAL written.  Then the keyframe planner's costs, fitted to
 * those moves (and to capturing the changed pages through pageinspect, as
 * an incremental keyframe would), and per shape whether the planner picks
 * the path that was actually faster.
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../test/test_util.h"
#include "../kfplan.h"

#define BLCK 8192
#define FORK_MAIN 0
#define FORK_VM 2
#define NREL 3

typedef struct Opts
{
	const char *conninfo;
	const char *pgdata;
	const char *staging;
	int64_t		rows;
	int			pad;
	int			reps;
	const char *shapes;
	bool		warm;
	const char *compressions;
} Opts;

static Opts o = {"host=/tmp port=5499 dbname=postgres", NULL,
	"/home/dustin/pgv2-bench/kfbench", 1000000, 100, 3,
"sparse:100,sparse:1000,sparse:10000,sparse:100000,cluster:1000,cluster:10000,cluster:100000,all:0", false, "pglz,off"};

static PGconn *db;
static char *comps[4];			/* wal_compression settings to measure */
static int	ncomp;

/* A relation's fork as a page array. */
typedef struct Fork
{
	uint8_t    *pages;
	uint32_t	n;
} Fork;

typedef struct Rel
{
	const char *name;
	uint32_t	oid;
	char		path[512];
	Fork		f[3];			/* by fork number: main, (fsm unused), vm */
} Rel;

typedef struct Snap
{
	Fork		f[NREL][3];
} Snap;

static Rel rels[NREL] = {{.name = "t"}, {.name = "t_pkey"}, {.name = "t_k"}};

/* ------------------------------------------------------------------ sql */

static PGresult *
qp(const char *sql, int n, const char *const *vals, const int *lens, const int *fmts)
{
	PGresult   *r = PQexecParams(db, sql, n, NULL, vals, lens, fmts, 0);
	ExecStatusType st = PQresultStatus(r);

	if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "\nquery failed: %s\n%s\n", sql, PQerrorMessage(db));
		exit(1);
	}
	return r;
}

static PGresult *
q(const char *sql)
{
	return qp(sql, 0, NULL, NULL, NULL);
}

static void
qx(const char *sql)
{
	PQclear(q(sql));
}

static uint64_t
wal_lsn(void)
{
	PGresult   *r = q("SELECT pg_current_wal_insert_lsn() - '0/0'::pg_lsn");
	uint64_t	v = strtoull(PQgetvalue(r, 0, 0), NULL, 10);

	PQclear(r);
	return v;
}

/* ----------------------------------------------------------- the table */

static void
k0_pad0(int64_t id, int32_t *k, char *pad)
{
	char		digits[24];
	int			n = snprintf(digits, sizeof(digits), "%lld", (long long) id);
	int			fill = o.pad > n ? o.pad - n : 0;

	*k = (int32_t) ((id * 7919) % 1000003);
	memset(pad, 'x', (size_t) fill);
	memcpy(pad + fill, digits, (size_t) (o.pad > n ? n : o.pad));
	pad[o.pad] = '\0';
}

static void
setup(void)
{
	char		sql[512];

	qx("CREATE EXTENSION IF NOT EXISTS amcheck");
	qx("CREATE EXTENSION IF NOT EXISTS pageinspect");
	qx("DROP EXTENSION IF EXISTS prolly_pages");
	qx("CREATE EXTENSION prolly_pages");
	qx("DROP TABLE IF EXISTS t");
	qx("CREATE TABLE t (id bigint, k int NOT NULL, v int NOT NULL, pad text NOT NULL) "
	   "WITH (autovacuum_enabled = false, toast.autovacuum_enabled = false)");
	snprintf(sql, sizeof(sql),
			 "INSERT INTO t SELECT g, (g * 7919) %% 1000003, 0, lpad(g::text, %d, 'x') "
			 "FROM generate_series(1::bigint, %lld) g", o.pad, (long long) o.rows);
	qx(sql);
	qx("ALTER TABLE t ADD PRIMARY KEY (id)");
	qx("CREATE INDEX t_k ON t (k)");
	qx("VACUUM (FREEZE, ANALYZE) t");
	for (int i = 0; i < NREL; i++)
	{
		PGresult   *r;

		snprintf(sql, sizeof(sql), "SELECT '%s'::regclass::oid, pg_relation_filepath('%s')",
				 rels[i].name, rels[i].name);
		r = q(sql);
		rels[i].oid = (uint32_t) strtoul(PQgetvalue(r, 0, 0), NULL, 10);
		snprintf(rels[i].path, sizeof(rels[i].path), "%s/%s", o.pgdata, PQgetvalue(r, 0, 1));
		PQclear(r);
	}
}

/* --------------------------------------------------------------- pages */

static void
read_fork(const Rel *r, int fork, Fork *out)
{
	size_t		cap = 0;

	out->pages = NULL;
	out->n = 0;
	for (int seg = 0;; seg++)
	{
		char		path[600];
		int			fd;
		ssize_t		got;

		snprintf(path, sizeof(path), "%s%s", r->path, fork == FORK_VM ? "_vm" : "");
		if (seg)
			snprintf(path + strlen(path), sizeof(path) - strlen(path), ".%d", seg);
		if ((fd = open(path, O_RDONLY)) < 0)
			break;
		for (;;)
		{
			px_grow((void **) &out->pages, &cap, ((size_t) out->n + 256) * BLCK, 1);
			got = read(fd, out->pages + (size_t) out->n * BLCK, 256 * BLCK);
			if (got <= 0)
				break;
			CHECK(got % BLCK == 0);
			out->n += (uint32_t) (got / BLCK);
		}
		close(fd);
	}
}

/* The files as of a checkpoint. */
static void
snapshot(Snap *s)
{
	qx("CHECKPOINT");
	for (int i = 0; i < NREL; i++)
	{
		read_fork(&rels[i], FORK_MAIN, &s->f[i][FORK_MAIN]);
		read_fork(&rels[i], FORK_VM, &s->f[i][FORK_VM]);
	}
}

static void
snap_free(Snap *s)
{
	for (int i = 0; i < NREL; i++)
	{
		free(s->f[i][FORK_MAIN].pages);
		free(s->f[i][FORK_VM].pages);
	}
}

/* pd_lsn and pd_checksum change with every write; they are not content. */
static bool
same_page(const uint8_t *a, const uint8_t *b)
{
	return memcmp(a + 10, b + 10, BLCK - 10) == 0;
}

/* What moving from `from` to `to` writes: per relation and fork. */
typedef struct Move
{
	uint32_t   *blocks[NREL][3];
	uint32_t	n[NREL][3];
	uint32_t	truncate_to[NREL];	/* UINT32_MAX: no truncation */
	uint64_t	pages;
} Move;

static void
plan_move(const Snap *from, const Snap *to, Move *m)
{
	memset(m, 0, sizeof(*m));
	for (int i = 0; i < NREL; i++)
	{
		m->truncate_to[i] = from->f[i][FORK_MAIN].n > to->f[i][FORK_MAIN].n ?
			to->f[i][FORK_MAIN].n : UINT32_MAX;
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
		{
			const Fork *a = &from->f[i][f],
					   *b = &to->f[i][f];
			size_t		cap = 0;

			/* map pages past the target's end are cleared, not truncated */
			for (uint32_t blk = 0; blk < (f == FORK_VM && a->n > b->n ? a->n : b->n); blk++)
			{
				static const uint8_t zero[BLCK];
				const uint8_t *want = blk < b->n ? b->pages + (size_t) blk * BLCK : zero;

				if (blk < a->n && same_page(a->pages + (size_t) blk * BLCK, want))
					continue;
				px_grow((void **) &m->blocks[i][f], &cap, m->n[i][f] + 1, sizeof(uint32_t));
				m->blocks[i][f][m->n[i][f]++] = blk;
				m->pages++;
			}
		}
	}
}

static void
move_free(Move *m)
{
	for (int i = 0; i < NREL; i++)
		for (int f = 0; f < 3; f++)
			free(m->blocks[i][f]);
}

static const uint8_t *
target_page(const Snap *to, int i, int f, uint32_t blk)
{
	static const uint8_t zero[BLCK];

	return blk < to->f[i][f].n ? to->f[i][f].pages + (size_t) blk * BLCK : zero;
}

static void
int_array(Buf *b, const uint32_t *v, uint32_t n)
{
	b->len = 0;
	buf_append_byte(b, '{');
	for (uint32_t i = 0; i < n; i++)
	{
		char		num[16];
		int			len = snprintf(num, sizeof(num), "%s%u", i ? "," : "", v[i]);

		buf_append(b, num, (size_t) len);
	}
	buf_append(b, "}", 2);
}

/* The staging file for one relation and fork: the target pages, in order. */
static void
stage_path(char *out, size_t len, const char *tag, int i, int f)
{
	snprintf(out, len, "%s/%s-%d-%d", o.staging, tag, i, f);
}

static void
write_staging(const char *tag, const Snap *to, const Move *m)
{
	for (int i = 0; i < NREL; i++)
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
		{
			char		path[600];
			int			fd;

			stage_path(path, sizeof(path), tag, i, f);
			fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			CHECK(fd >= 0);
			for (uint32_t j = 0; j < m->n[i][f]; j++)
				CHECK(write(fd, target_page(to, i, f, m->blocks[i][f][j]), BLCK) == BLCK);
			close(fd);
		}
}

typedef enum Method
{
	M_REPLAY,
	M_PAGES_SQL,
	M_PAGES_FILE,
	M_PAGES_BATCH,
	M_COUNT
} Method;

static const char *method_name[] = {"replay", "pages-sql", "pages-file", "pages-batch"};

/* Writes the move's pages (staging written already for the file methods). */
static void
apply_pages(const char *tag, const Snap *to, const Move *m, Method how)
{
	Buf			arr = BUF_INIT,
				pages = BUF_INIT;

	qx("BEGIN");
	qx("LOCK TABLE t IN ACCESS EXCLUSIVE MODE");
	for (int i = 0; i < NREL; i++)
	{
		char		oid_s[16];

		snprintf(oid_s, sizeof(oid_s), "%u", rels[i].oid);
		if (m->truncate_to[i] != UINT32_MAX)
		{
			char		sql[96];

			snprintf(sql, sizeof(sql), "SELECT prolly_truncate(%u, %u)", rels[i].oid,
					 m->truncate_to[i]);
			qx(sql);
		}
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
		{
			const char *fork = f == FORK_VM ? "vm" : "main";

			if (m->n[i][f] == 0)
				continue;
			if (how == M_PAGES_SQL)
			{
				for (uint32_t a = 0; a < m->n[i][f]; a += 2000)
				{
					uint32_t	n = m->n[i][f] - a < 2000 ? m->n[i][f] - a : 2000;
					const char *vals[4] = {oid_s, fork, NULL, NULL};
					int			lens[4] = {0, 0, 0, 0},
								fmts[4] = {0, 0, 0, 1};

					int_array(&arr, m->blocks[i][f] + a, n);
					pages.len = 0;
					for (uint32_t j = 0; j < n; j++)
						buf_append(&pages, target_page(to, i, f, m->blocks[i][f][a + j]), BLCK);
					vals[2] = (const char *) arr.data;
					vals[3] = (const char *) pages.data;
					lens[3] = (int) pages.len;
					PQclear(qp("SELECT prolly_put_pages($1::oid::regclass, $2, $3::int4[], $4::bytea)",
							   4, vals, lens, fmts));
				}
			}
			else
			{
				char		path[600];
				const char *vals[5] = {oid_s, fork, NULL, path,
				how == M_PAGES_BATCH ? "true" : "false"};

				stage_path(path, sizeof(path), tag, i, f);
				int_array(&arr, m->blocks[i][f], m->n[i][f]);
				vals[2] = (const char *) arr.data;
				PQclear(qp("SELECT prolly_put_pages_file($1::oid::regclass, $2, $3::int4[], $4, $5::bool)",
						   5, vals, NULL, NULL));
			}
		}
	}
	qx("COMMIT");
	buf_free(&arr);
	buf_free(&pages);
}

/* Puts the files back exactly as `to`, untimed. */
static void
restore_exactly(const Snap *to)
{
	Snap		cur;
	Move		m;

	snapshot(&cur);
	plan_move(&cur, to, &m);
	write_staging("reset", to, &m);
	apply_pages("reset", to, &m, M_PAGES_BATCH);
	move_free(&m);
	snap_free(&cur);
}

/* ------------------------------------------------------------- changes */

typedef struct Shape
{
	char		kind[16];
	int64_t		n;
	int64_t    *ids;
} Shape;

static void
shape_ids(Shape *s)
{
	if (strcmp(s->kind, "all") == 0)
		s->n = o.rows;
	s->ids = px_malloc((size_t) s->n * sizeof(int64_t));
	if (strcmp(s->kind, "sparse") == 0)
	{
		PHashMap	seen;

		phmap_init(&seen);
		for (int64_t i = 0; i < s->n;)
		{
			int64_t		id = 1 + (int64_t) rnd_below((uint64_t) o.rows);
			PHash		k = {{0}};
			bool		created;

			memcpy(k.b, &id, sizeof(id));
			phmap_ref(&seen, &k, &created);
			if (created)
				s->ids[i++] = id;
		}
		phmap_free(&seen);
	}
	else
	{
		int64_t		start = strcmp(s->kind, "all") == 0 ? 1 :
			1 + (int64_t) rnd_below((uint64_t) (o.rows - s->n));

		for (int64_t i = 0; i < s->n; i++)
			s->ids[i] = start + i;
	}
}

static void
id_array(Buf *b, const int64_t *ids, int64_t n)
{
	b->len = 0;
	buf_append_byte(b, '{');
	for (int64_t i = 0; i < n; i++)
	{
		char		num[24];
		int			len = snprintf(num, sizeof(num), "%s%lld", i ? "," : "", (long long) ids[i]);

		buf_append(b, num, (size_t) len);
	}
	buf_append(b, "}", 2);
}

/* S0 -> S1: the shape's rows get a new k and v. */
static void
change(const Shape *s)
{
	Buf			ids = BUF_INIT;
	const char *vals[1];

	id_array(&ids, s->ids, s->n);
	vals[0] = (const char *) ids.data;
	PQclear(qp("UPDATE t SET k = (k + 1) % 1000003, v = v + 1 WHERE id = ANY($1::bigint[])",
			   1, vals, NULL, NULL));
	buf_free(&ids);
}

/* S1 -> S0 by rows: the shape's rows set back to their S0 values. */
static void
replay(const Shape *s)
{
	Buf			ids = BUF_INIT,
				ks = BUF_INIT,
				pads = BUF_INIT;
	char	   *pad = px_malloc((size_t) o.pad + 1);
	const char *vals[3];

	id_array(&ids, s->ids, s->n);
	buf_append_byte(&ks, '{');
	buf_append_byte(&pads, '{');
	for (int64_t i = 0; i < s->n; i++)
	{
		int32_t		k;
		char		num[16];
		int			len;

		k0_pad0(s->ids[i], &k, pad);
		len = snprintf(num, sizeof(num), "%s%d", i ? "," : "", k);
		buf_append(&ks, num, (size_t) len);
		if (i)
			buf_append_byte(&pads, ',');
		buf_append(&pads, pad, (size_t) o.pad);
	}
	buf_append(&ks, "}", 2);
	buf_append(&pads, "}", 2);
	vals[0] = (const char *) ids.data;
	vals[1] = (const char *) ks.data;
	vals[2] = (const char *) pads.data;
	qx("BEGIN");
	PQclear(qp("INSERT INTO t SELECT i, k, 0, p FROM unnest($1::bigint[], $2::int[], $3::text[]) u(i, k, p) "
			   "ON CONFLICT (id) DO UPDATE SET k = excluded.k, v = excluded.v, pad = excluded.pad",
			   3, vals, NULL, NULL));
	qx("COMMIT");
	buf_free(&ids);
	buf_free(&ks);
	buf_free(&pads);
	free(pad);
}

/* ---------------------------------------------------------------- check */

static char s0_sum[64];

static void
table_sum(char *out)
{
	PGresult   *r = q("SELECT count(*) || ':' || sum(hashtextextended(id || ',' || k || ',' || v || ',' || pad, 0)) FROM t");

	snprintf(out, 64, "%s", PQgetvalue(r, 0, 0));
	PQclear(r);
}

static void
verify_s0(const char *what)
{
	char		sum[64];

	table_sum(sum);
	if (strcmp(sum, s0_sum) != 0)
	{
		fprintf(stderr, "%s: rows differ from S0 (%s vs %s)\n", what, sum, s0_sum);
		exit(1);
	}
	qx("SELECT bt_index_check('t_pkey', true), bt_index_check('t_k', true)");
}

/* ----------------------------------------------------------------- main */

typedef struct Result
{
	char		label[32];
	uint64_t	rows,
				pages;
	double		ms[4][M_COUNT];	/* by wal_compression, as listed */
	double		wal_mb[4][M_COUNT];
	double		capture_ms;
} Result;

/* Reads the move's changed pages the way an incremental capture does. */
static double
capture(const Move *m)
{
	Buf			arr = BUF_INIT;
	double		t = now_sec();

	for (int i = 0; i < NREL; i++)
		for (int f = FORK_MAIN; f <= FORK_VM; f += FORK_VM)
			for (uint32_t a = 0; a < m->n[i][f]; a += 2000)
			{
				uint32_t	n = m->n[i][f] - a < 2000 ? m->n[i][f] - a : 2000;
				const char *vals[3] = {rels[i].name, f == FORK_VM ? "vm" : "main", NULL};

				int_array(&arr, m->blocks[i][f] + a, n);
				vals[2] = (const char *) arr.data;
				PQclear(PQexecParams(db, "SELECT get_raw_page($1, $2, b) FROM unnest($3::int8[]) b",
									 3, NULL, vals, NULL, NULL, 1));
			}
	buf_free(&arr);
	return (now_sec() - t) * 1e3;
}

static int
dbl_cmp(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return (x > y) - (x < y);
}

int
main(int argc, char **argv)
{
	static struct option opts[] = {
		{"conn", 1, 0, 'c'}, {"pgdata", 1, 0, 'D'}, {"staging", 1, 0, 's'},
		{"rows", 1, 0, 'r'}, {"pad", 1, 0, 'p'}, {"reps", 1, 0, 'n'},
		{"shapes", 1, 0, 'h'}, {"warm", 0, 0, 'w'}, {"compressions", 1, 0, 'z'},
		{0, 0, 0, 0}
	};
	int			ch;
	Snap		s0;
	char	   *shapes;
	Result		res[32];
	int			nres = 0;

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'c': o.conninfo = optarg; break;
			case 'D': o.pgdata = optarg; break;
			case 's': o.staging = optarg; break;
			case 'r': o.rows = strtoll(optarg, NULL, 10); break;
			case 'p': o.pad = atoi(optarg); break;
			case 'n': o.reps = atoi(optarg); break;
			case 'h': o.shapes = optarg; break;
			case 'w': o.warm = true; break;
			case 'z': o.compressions = optarg; break;
			default:
				fprintf(stderr, "kfbench --pgdata DIR [--conn STR] [--staging DIR] [--rows N]\n"
						"        [--pad B] [--reps N] [--shapes kind:n,...] [--warm]\n"
						"        [--compressions off,pglz,lz4,zstd]\n");
				return 2;
		}
	if (o.pgdata == NULL)
	{
		fprintf(stderr, "kfbench: --pgdata is required\n");
		return 2;
	}
	{
		char	   *list = px_memdup(o.compressions, strlen(o.compressions) + 1);

		for (char *t = strtok(list, ","); t && ncomp < 4; t = strtok(NULL, ","))
			comps[ncomp++] = px_memdup(t, strlen(t) + 1);
		free(list);
		CHECK(ncomp > 0);
	}
	mkdir(o.staging, 0755);
	db = PQconnectdb(o.conninfo);
	CHECK(PQstatus(db) == CONNECTION_OK);
	rng_seed(31);
	setup();
	snapshot(&s0);
	table_sum(s0_sum);
	{
		uint64_t	pages = 0;

		for (int i = 0; i < NREL; i++)
			pages += s0.f[i][FORK_MAIN].n;
		printf("# %lld rows, pad %d: %llu pages in t and its two indexes; %s\n",
			   (long long) o.rows, o.pad, (unsigned long long) pages,
			   o.warm ? "warm: no checkpoint before a move" : "a checkpoint before every move");
	}
	printf("%-16s %8s %8s  %-12s %-5s %10s %10s %10s %9s\n", "shape", "rows", "pages",
		   "method", "comp", "ms", "us/row", "us/page", "wal_MB");

	shapes = px_memdup(o.shapes, strlen(o.shapes) + 1);
	for (char *tok = strtok(shapes, ","); tok; tok = strtok(NULL, ","))
	{
		Shape		s;
		Snap		s1;
		char		label[32];
		Move		back;
		char	   *colon = strchr(tok, ':');

		memset(&s, 0, sizeof(s));
		CHECK(colon != NULL);
		snprintf(s.kind, sizeof(s.kind), "%.*s", (int) (colon - tok), tok);
		s.n = strtoll(colon + 1, NULL, 10);
		snprintf(label, sizeof(label), "%s", tok);
		shape_ids(&s);

		change(&s);
		snapshot(&s1);
		plan_move(&s1, &s0, &back);
		write_staging("back", &s0, &back);
		CHECK(nres < 32);
		memset(&res[nres], 0, sizeof(Result));
		snprintf(res[nres].label, sizeof(res[nres].label), "%s", label);
		res[nres].rows = (uint64_t) s.n;
		res[nres].pages = back.pages;
		/* S1's changed pages, read as capturing a keyframe at S1 would */
		res[nres].capture_ms = capture(&back);

		for (int comp = 0; comp < ncomp; comp++)
			for (Method mth = 0; mth < M_COUNT; mth++)
			{
				double		ms[16];
				uint64_t	wal = 0;
				int			reps = o.reps < 16 ? o.reps : 16;

				/* past the first setting, only replay and the best page path */
				if (comp > 0 && mth != M_REPLAY && mth != M_PAGES_BATCH)
					continue;
				for (int rep = 0; rep < reps; rep++)
				{
					double		t;
					uint64_t	w;

					restore_exactly(&s1);
					{
						char		set[64];

						snprintf(set, sizeof(set), "SET wal_compression = %s", comps[comp]);
						qx(set);
					}
					if (!o.warm)
						qx("CHECKPOINT");
					w = wal_lsn();
					t = now_sec();
					if (mth == M_REPLAY)
						replay(&s);
					else
						apply_pages("back", &s0, &back, mth);
					ms[rep] = (now_sec() - t) * 1e3;
					wal += wal_lsn() - w;
					qx("RESET wal_compression");
					verify_s0(method_name[mth]);
				}
				qsort(ms, (size_t) reps, sizeof(double), dbl_cmp);
				res[nres].ms[comp][mth] = ms[reps / 2];
				res[nres].wal_mb[comp][mth] = wal / (double) reps / 1048576.0;
				printf("%-16s %8lld %8llu  %-12s %-5s %10.1f %10.2f %10.2f %9.1f\n",
					   label, (long long) s.n,
					   (unsigned long long) back.pages, method_name[mth], comps[comp],
					   ms[reps / 2], ms[reps / 2] * 1e3 / (double) s.n,
					   back.pages ? ms[reps / 2] * 1e3 / (double) back.pages : 0,
					   wal / (double) reps / 1048576.0);
				fflush(stdout);
			}
		nres++;
		/* back to S0 exactly for the next shape */
		restore_exactly(&s0);
		verify_s0("reset");
		move_free(&back);
		snap_free(&s1);
		free(s.ids);
	}
	free(shapes);
	snap_free(&s0);

	/* the planner's costs, fitted to the moves, and whether it picks right */
	/*
	 * The planner on these moves: calibrated on all of them, its costs; and
	 * each move predicted from all the others, whether it picks the faster
	 * path and what it loses when it does not.
	 */
	for (int comp = 0; comp < ncomp; comp++)
	{
		static const KfCosts poor = {
			.replay_fixed_us = 1000, .row_us = 8, .restore_fixed_us = 1000, .page_us = 150,
			.capture_page_us = 5
		};
		KfCalibration cal = {.forget = 1, .prior_weight = 0.5, .explore_every = 0};
		KfPlanner  *all = kf_calibrating_planner(&poor, &cal);
		KfCosts		k;
		int			right = 0;
		double		lost = 0,
					best = 0;

		for (int i = 0; i < nres; i++)
		{
			KfMove		m = {res[i].rows, res[i].pages, o.warm ? res[i].pages : 0};

			all->ops->observe_replay(all, &m, res[i].ms[comp][M_REPLAY] * 1e3);
			all->ops->observe_restore(all, res[i].pages, res[i].ms[comp][M_PAGES_BATCH] * 1e3);
		}
		all->ops->costs(all, &k);
		printf("\n# wal_compression %s, %s: replay %.0f us + %.2f us/row + %.2f us/page%s; "
			   "restore %.0f us + %.2f us/page\n", comps[comp], o.warm ? "warm" : "cold",
			   k.replay_fixed_us, k.row_us, o.warm ? k.replay_warm_page_us : k.replay_page_us,
			   o.warm ? " (warm)" : "", k.restore_fixed_us, k.page_us);
		printf("%-16s %8s %8s %9s %10s %10s  %-8s %-8s\n", "shape", "rows", "pages",
			   "pages/row", "replay_ms", "restore_ms", "faster", "planner");
		for (int out = 0; out < nres; out++)
		{
			KfPlanner  *p = kf_calibrating_planner(&poor, &cal);
			KfMove		m = {res[out].rows, res[out].pages, o.warm ? res[out].pages : 0};
			KfCandidate c;
			KfChoice	chs;
			double		r = res[out].ms[comp][M_REPLAY],
						t = res[out].ms[comp][M_PAGES_BATCH];

			for (int i = 0; i < nres; i++)
				if (i != out)
				{
					KfMove		mi = {res[i].rows, res[i].pages, o.warm ? res[i].pages : 0};

					p->ops->observe_replay(p, &mi, res[i].ms[comp][M_REPLAY] * 1e3);
					p->ops->observe_restore(p, res[i].pages, res[i].ms[comp][M_PAGES_BATCH] * 1e3);
				}
			memset(&c, 0, sizeof(c));
			c.pages = res[out].pages;
			chs = p->ops->choose(p, &m, &c, 1);
			right += (chs.path == KF_RESTORE) == (t < r);
			lost += (chs.path == KF_RESTORE ? t : r) - (t < r ? t : r);
			best += t < r ? t : r;
			printf("%-16s %8llu %8llu %9.3f %10.1f %10.1f  %-8s %-8s\n", res[out].label,
				   (unsigned long long) res[out].rows, (unsigned long long) res[out].pages,
				   (double) res[out].pages / (double) res[out].rows, r, t,
				   t < r ? "restore" : "replay", chs.path == KF_RESTORE ? "restore" : "replay");
			p->ops->destroy(p);
		}
		printf("# each predicted from the others: %d of %d right, %.2f%% of the best time lost\n",
			   right, nres, 100 * lost / best);
		all->ops->destroy(all);
	}

	{
		double		ms = 0,
					pages = 0;

		for (int i = 0; i < nres; i++)
		{
			ms += res[i].capture_ms;
			pages += (double) res[i].pages;
		}
		printf("\n# capturing the changed pages through pageinspect: %.2f us/page\n",
			   pages > 0 ? ms * 1e3 / pages : 0);
	}

	/* every setting side by side: replay ms, restore ms, and their WAL */
	printf("\n# by wal_compression: replay ms / restore ms (WAL MB replay / restore)\n%-16s", "shape");
	for (int comp = 0; comp < ncomp; comp++)
		printf(" %30s", comps[comp]);
	printf("\n");
	for (int i = 0; i < nres; i++)
	{
		printf("%-16s", res[i].label);
		for (int comp = 0; comp < ncomp; comp++)
		{
			char		cell[64];

			snprintf(cell, sizeof(cell), "%.0f / %.0f (%.0f / %.0f)", res[i].ms[comp][M_REPLAY],
					 res[i].ms[comp][M_PAGES_BATCH], res[i].wal_mb[comp][M_REPLAY],
					 res[i].wal_mb[comp][M_PAGES_BATCH]);
			printf(" %30s", cell);
		}
		printf("\n");
	}
	PQfinish(db);
	return 0;
}
