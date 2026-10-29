/*
 * xidbench.c
 *	  Keyframes across the freeze horizon: a raw page image names
 *	  transactions, which the server forgets as its horizon moves; a frozen
 *	  image (prolly_get_pages_frozen) names none.
 *
 * Runs against a throwaway cluster of its own (made on first use, with
 * fsync off), since it burns millions of transaction IDs and freezes
 * template0.  Each case takes an image of table t (its heap, TOAST, their
 * indexes and the visibility map) both raw and frozen, changes the
 * server's state, then puts each image back and checks it:
 *	horizon		3M transaction IDs later, every database frozen and the
 *				commit log truncated: the raw image must fail, the frozen
 *				one must come back exactly
 *	inflight	a transaction in flight at capture (inserts, HOT and
 *				non-HOT updates, deletes) commits afterwards: the raw image
 *				shows its rows, the frozen one must not
 *	multixact	two sessions hold share locks at capture: the frozen image
 *				must name no multixact
 *	reuse		after a frozen restore the table takes inserts, updates,
 *				deletes and VACUUM like any other
 * "Exactly" is: the rows' checksum equals the one taken with the capture's
 * snapshot, amcheck's verify_heapam finds nothing, bt_index_check with
 * heapallindexed passes on every index, counts through each index agree,
 * and VACUUM runs clean.
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../test/test_util.h"

#define BLCK 8192

static const char *pgbin = PGBIN;
static const char *datadir = "/home/dustin/pgv2-bench/xiddb";
static int	failures;
static bool expect_failure;		/* reporting checks meant to fail */

/* ------------------------------------------------------------------ sql */

static PGconn *
connect_to(const char *db)
{
	char		ci[256];
	PGconn	   *c;

	snprintf(ci, sizeof(ci), "host=/tmp port=5497 user=postgres dbname=%s", db);
	c = PQconnectdb(ci);
	if (PQstatus(c) != CONNECTION_OK)
	{
		fprintf(stderr, "connect %s: %s", db, PQerrorMessage(c));
		exit(1);
	}
	PQclear(PQexec(c, "SET client_min_messages = warning"));
	return c;
}

static PGresult *
qp(PGconn *c, const char *sql, int n, const char *const *vals, const int *lens,
   const int *fmts, int resfmt)
{
	PGresult   *r = PQexecParams(c, sql, n, NULL, vals, lens, fmts, resfmt);
	ExecStatusType st = PQresultStatus(r);

	if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "\nquery failed: %s\n%s\n", sql, PQerrorMessage(c));
		exit(1);
	}
	return r;
}

static void
qx(PGconn *c, const char *sql)
{
	PQclear(qp(c, sql, 0, NULL, NULL, NULL, 0));
}

/* Runs sql; returns NULL on success, else the error (static). */
static const char *
try(PGconn *c, const char *sql)
{
	static char err[512];
	PGresult   *r = PQexec(c, sql);
	ExecStatusType st = PQresultStatus(r);

	PQclear(r);
	if (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK)
		return NULL;
	snprintf(err, sizeof(err), "%s", PQerrorMessage(c));
	for (char *p = err; *p; p++)
		if (*p == '\n')
			*p = ' ';
	return err;
}

static char *
q1(PGconn *c, const char *sql)
{
	PGresult   *r = qp(c, sql, 0, NULL, NULL, NULL, 0);
	char	   *v = px_memdup(PQgetvalue(r, 0, 0), strlen(PQgetvalue(r, 0, 0)) + 1);

	PQclear(r);
	return v;
}

/* -------------------------------------------------------------- cluster */

static void
sh(const char *cmd)
{
	if (system(cmd) != 0)
	{
		fprintf(stderr, "failed: %s\n", cmd);
		exit(1);
	}
}

static void
start_cluster(void)
{
	char		cmd[1024];
	struct stat st;

	if (stat(datadir, &st) != 0)
	{
		snprintf(cmd, sizeof(cmd), "%s/initdb -D %s -U postgres --no-sync -A trust >/dev/null", pgbin,
				 datadir);
		sh(cmd);
		snprintf(cmd, sizeof(cmd),
				 "printf \"port = 5497\\nfsync = off\\nwal_log_hints = on\\nautovacuum = off\\n"
				 "shared_buffers = 256MB\\nmax_wal_size = 8GB\\nunix_socket_directories = '/tmp'\\n\" "
				 ">> %s/postgresql.conf", datadir);
		sh(cmd);
	}
	snprintf(cmd, sizeof(cmd), "%s/pg_ctl -D %s -l %s/server.log -w status >/dev/null 2>&1 || "
			 "%s/pg_ctl -D %s -l %s/server.log -w start >/dev/null", pgbin, datadir, datadir,
			 pgbin, datadir, datadir);
	sh(cmd);
}

/* --------------------------------------------------------------- images */

#define NREL 6

typedef struct Rel
{
	char		name[128];		/* regclass text */
	const char *fork;
	uint32_t	nblocks;
	uint8_t    *pages;
} Rel;

typedef struct Image
{
	Rel			r[NREL];
	char		sum[128];		/* rows as of the capture */
} Image;

static void
rel_names(PGconn *c, Image *im)
{
	char	   *toast = q1(c, "SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 't'::regclass");
	char	   *tidx = q1(c, "SELECT indexrelid::regclass FROM pg_index WHERE indrelid = "
						  "(SELECT reltoastrelid FROM pg_class WHERE oid = 't'::regclass)");

	snprintf(im->r[0].name, 128, "t");
	snprintf(im->r[1].name, 128, "t_pkey");
	snprintf(im->r[2].name, 128, "t_v");
	snprintf(im->r[3].name, 128, "%s", toast);
	snprintf(im->r[4].name, 128, "%s", tidx);
	snprintf(im->r[5].name, 128, "t");
	for (int i = 0; i < NREL; i++)
		im->r[i].fork = i == 5 ? "vm" : "main";
	free(toast);
	free(tidx);
}

static const char *sum_sql =
"SELECT count(*) || ':' || coalesce(sum(hashtextextended(id || ',' || v || ',' || "
"md5(pad), 0)), 0) FROM t";

/*
 * Takes an image of t on c, which must be inside the transaction whose
 * snapshot the image is to show (REPEATABLE READ).  frozen reads through
 * prolly_get_pages_frozen; otherwise the pages are read as they are.
 */
static void
capture(PGconn *c, Image *im, bool frozen)
{
	char		sql[512];

	memset(im, 0, sizeof(*im));
	rel_names(c, im);
	for (int i = 0; i < NREL; i++)
	{
		Rel		   *r = &im->r[i];
		PGresult   *res;
		char	   *n;

		snprintf(sql, sizeof(sql), "SELECT pg_relation_size('%s', '%s') / %d", r->name, r->fork,
				 BLCK);
		n = q1(c, sql);
		r->nblocks = (uint32_t) strtoul(n, NULL, 10);
		free(n);
		if (r->nblocks == 0)
			continue;
		snprintf(sql, sizeof(sql),
				 frozen ? "SELECT prolly_get_pages_frozen('%s', '%s', array(SELECT generate_series(0, %u - 1)))"
				 : "SELECT string_agg(get_raw_page('%s', '%s', b), ''::bytea ORDER BY b) "
				 "FROM generate_series(0, %u - 1) b",
				 r->name, r->fork, r->nblocks);
		res = qp(c, sql, 0, NULL, NULL, NULL, 1);
		CHECK((size_t) PQgetlength(res, 0, 0) == (size_t) r->nblocks * BLCK);
		r->pages = px_memdup(PQgetvalue(res, 0, 0), (size_t) r->nblocks * BLCK);
		PQclear(res);
	}
	{
		char	   *s = q1(c, sum_sql);

		snprintf(im->sum, sizeof(im->sum), "%s", s);
		free(s);
	}
}

static void
image_free(Image *im)
{
	for (int i = 0; i < NREL; i++)
		free(im->r[i].pages);
}

/* Puts the image back: every page, each relation cut to its size. */
static void
restore(PGconn *c, const Image *im)
{
	char		sql[512];

	qx(c, "BEGIN");
	qx(c, "LOCK TABLE t IN ACCESS EXCLUSIVE MODE");
	for (int i = 0; i < NREL; i++)
	{
		const Rel  *r = &im->r[i];
		Buf			arr = BUF_INIT;
		const char *vals[4];
		int			lens[4] = {0, 0, 0, 0},
					fmts[4] = {0, 0, 0, 1};

		if (strcmp(r->fork, "main") == 0)
		{
			snprintf(sql, sizeof(sql), "SELECT prolly_truncate('%s'::regclass, %u)", r->name,
					 r->nblocks);
			qx(c, sql);
		}
		if (r->nblocks == 0)
			continue;
		buf_append_byte(&arr, '{');
		for (uint32_t b = 0; b < r->nblocks; b++)
		{
			char		num[16];
			int			len = snprintf(num, sizeof(num), "%s%u", b ? "," : "", b);

			buf_append(&arr, num, (size_t) len);
		}
		buf_append(&arr, "}", 2);
		vals[0] = r->name;
		vals[1] = r->fork;
		vals[2] = (const char *) arr.data;
		vals[3] = (const char *) r->pages;
		lens[3] = (int) ((size_t) r->nblocks * BLCK);
		PQclear(qp(c, "SELECT prolly_put_pages($1::regclass, $2, $3::int4[], $4::bytea)", 4, vals,
				   lens, fmts, 0));
		buf_free(&arr);
	}
	qx(c, "COMMIT");
}

/* ---------------------------------------------------------------- checks */

static void
report(const char *cse, const char *what, bool ok, const char *detail)
{
	if (expect_failure)
	{
		/* a check on the raw image: failing is what shows the hazard */
		printf("%-10s   raw: %-37s %s%s%.160s\n", cse, what, ok ? "passes" : "fails",
			   detail ? ": " : "", detail ? detail : "");
		fflush(stdout);
		return;
	}
	printf("%-10s %-44s %s%s%s\n", cse, what, ok ? "ok" : "FAILED",
		   detail ? ": " : "", detail ? detail : "");
	fflush(stdout);
	if (!ok)
		failures++;
}

/*
 * Everything that would notice a page the server cannot read: the rows,
 * the heap checker, every index with heapallindexed, counts through each
 * index, VACUUM.  Returns how many checks failed; with want_sum, the rows
 * must also match it.
 */
static int
check_table(PGconn *c, const char *cse, const char *want_sum, bool quiet)
{
	const char *err;
	int			bad = 0;
	char		detail[640];

	/* the rows */
	{
		PGresult   *r = PQexec(c, sum_sql);

		if (PQresultStatus(r) != PGRES_TUPLES_OK)
		{
			snprintf(detail, sizeof(detail), "%s", PQerrorMessage(c));
			for (char *p = detail; *p; p++)
				if (*p == '\n')
					*p = ' ';
			bad++;
			if (!quiet)
				report(cse, "rows read", false, detail);
		}
		else
		{
			bool		same = want_sum == NULL || strcmp(PQgetvalue(r, 0, 0), want_sum) == 0;

			snprintf(detail, sizeof(detail), "%s%s%s", PQgetvalue(r, 0, 0),
					 same ? "" : " vs ", same ? "" : want_sum);
			bad += !same;
			if (!quiet)
				report(cse, "rows as of the capture", same, same ? NULL : detail);
		}
		PQclear(r);
	}
	/* the heap checker */
	{
		PGresult   *r = PQexec(c, "SELECT count(*), min(msg) FROM verify_heapam('t', "
							   "check_toast := true)");

		if (PQresultStatus(r) != PGRES_TUPLES_OK)
		{
			snprintf(detail, sizeof(detail), "%s", PQerrorMessage(c));
			bad++;
			if (!quiet)
				report(cse, "verify_heapam", false, detail);
		}
		else
		{
			bool		ok = strcmp(PQgetvalue(r, 0, 0), "0") == 0;

			snprintf(detail, sizeof(detail), "%s problems, e.g. %s", PQgetvalue(r, 0, 0),
					 PQgetvalue(r, 0, 1));
			bad += !ok;
			if (!quiet)
				report(cse, "verify_heapam", ok, ok ? NULL : detail);
		}
		PQclear(r);
	}
	/* the indexes, each against the heap */
	err = try(c, "SELECT bt_index_check('t_pkey', true), bt_index_check('t_v', true)");
	bad += err != NULL;
	if (!quiet)
		report(cse, "bt_index_check heapallindexed", err == NULL, err);
	err = try(c, "DO $$ DECLARE a bigint; b bigint; s bigint; BEGIN "
			  "SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; "
			  "SELECT count(*) INTO a FROM t WHERE id > -1; "
			  "SELECT count(*) INTO b FROM t WHERE v > -1; "
			  "SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; "
			  "SET LOCAL enable_indexonlyscan = off; "
			  "SELECT count(*) INTO s FROM t; "
			  "IF a <> s OR b <> s THEN RAISE EXCEPTION 'counts % % %', a, b, s; END IF; END $$");
	bad += err != NULL;
	if (!quiet)
		report(cse, "counts through each index", err == NULL, err);
	/* a tuple with no updater is the latest of its row: it points at itself */
	{
		PGresult   *r = PQexec(c, "SELECT count(*) FROM generate_series(0, pg_relation_size('t') / 8192 - 1) b, "
							   "heap_page_items(get_raw_page('t', b::int)) "
							   "WHERE lp_flags = 1 AND t_xmax = '0' AND t_ctid <> format('(%s,%s)', b, lp)::tid");

		if (PQresultStatus(r) != PGRES_TUPLES_OK)
		{
			snprintf(detail, sizeof(detail), "%s", PQerrorMessage(c));
			bad++;
			if (!quiet)
				report(cse, "tuples without an updater point at themselves", false, detail);
		}
		else
		{
			bool		ok = strcmp(PQgetvalue(r, 0, 0), "0") == 0;

			bad += !ok;
			if (!quiet)
				report(cse, "tuples without an updater point at themselves", ok,
					   ok ? NULL : PQgetvalue(r, 0, 0));
		}
		PQclear(r);
	}
	err = try(c, "VACUUM t");
	bad += err != NULL;
	if (!quiet)
		report(cse, "VACUUM", err == NULL, err);
	return bad;
}

/* ----------------------------------------------------------------- setup */

static void
make_table(PGconn *c)
{
	qx(c, "DROP TABLE IF EXISTS t");
	qx(c, "CREATE TABLE t (id int PRIMARY KEY, v int NOT NULL, pad text NOT NULL) "
	   "WITH (autovacuum_enabled = false, fillfactor = 70)");
	qx(c, "CREATE INDEX t_v ON t (v)");
	/* in several transactions, so the pages name several */
	for (int a = 0; a < 5; a++)
	{
		char		sql[256];

		snprintf(sql, sizeof(sql),
				 "INSERT INTO t SELECT g, g, CASE WHEN g %% 50 = 0 THEN repeat(md5(g::text), 400) "
				 "ELSE md5(g::text) END FROM generate_series(%d, %d) g", a * 2000 + 1, a * 2000 + 2000);
		qx(c, sql);
	}
	/* HOT updates (pad only), non-HOT updates (the indexed v), deletes, TOAST rewrites */
	qx(c, "UPDATE t SET pad = pad || 'h' WHERE id % 7 = 0");
	qx(c, "UPDATE t SET v = v + 100000 WHERE id % 11 = 0");
	qx(c, "DELETE FROM t WHERE id % 13 = 0");
	qx(c, "UPDATE t SET pad = repeat(md5(id::text), 300) WHERE id % 100 = 1");
}

static void
burn_xids(PGconn *c, int n)
{
	char		sql[128];

	qx(c, "CREATE OR REPLACE PROCEDURE burn(n int) LANGUAGE plpgsql AS $$ "
	   "BEGIN FOR i IN 1..n LOOP PERFORM pg_current_xact_id(); COMMIT; END LOOP; END $$");
	snprintf(sql, sizeof(sql), "CALL burn(%d)", n);
	qx(c, sql);
}

/* Freezes every database, so datfrozenxid advances and the commit log is cut. */
static void
freeze_everything(PGconn *c)
{
	PGresult   *r;

	qx(c, "ALTER DATABASE template0 WITH ALLOW_CONNECTIONS true");
	r = qp(c, "SELECT datname FROM pg_database", 0, NULL, NULL, NULL, 0);
	for (int i = 0; i < PQntuples(r); i++)
	{
		PGconn	   *d = connect_to(PQgetvalue(r, i, 0));

		qx(d, "VACUUM (FREEZE)");
		PQfinish(d);
	}
	PQclear(r);
	qx(c, "ALTER DATABASE template0 WITH ALLOW_CONNECTIONS false");
	/* a second round, so vacuum sees the new datfrozenxid everywhere and truncates */
	r = qp(c, "SELECT datname FROM pg_database WHERE datallowconn", 0, NULL, NULL, NULL, 0);
	for (int i = 0; i < PQntuples(r); i++)
	{
		PGconn	   *d = connect_to(PQgetvalue(r, i, 0));

		qx(d, "VACUUM (FREEZE)");
		PQfinish(d);
	}
	PQclear(r);
	qx(c, "CHECKPOINT");
}

/* The oldest commit log segment left, as an xid. */
static uint64_t
oldest_clog_xid(void)
{
	char		path[600];
	DIR		   *d;
	struct dirent *e;
	unsigned long low = ~0UL;

	snprintf(path, sizeof(path), "%s/pg_xact", datadir);
	d = opendir(path);
	CHECK(d != NULL);
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
		{
			unsigned long seg = strtoul(e->d_name, NULL, 16);

			if (seg < low)
				low = seg;
		}
	closedir(d);
	return (uint64_t) low << 20;	/* 32 pages of 32768 transactions a segment */
}

/* ------------------------------------------------------------------ cases */

static void
case_horizon(PGconn *c)
{
	Image		raw,
				frozen;
	uint64_t	oldest_before,
				oldest_after;
	char	   *xmin;

	make_table(c);
	qx(c, "CHECKPOINT");
	qx(c, "BEGIN ISOLATION LEVEL REPEATABLE READ");
	capture(c, &raw, false);
	capture(c, &frozen, true);
	qx(c, "COMMIT");
	xmin = q1(c, "SELECT min(xmin::text::bigint) FROM t");
	oldest_before = oldest_clog_xid();

	burn_xids(c, 3000000);
	freeze_everything(c);
	oldest_after = oldest_clog_xid();
	{
		char		d[160];

		snprintf(d, sizeof(d), "oldest kept xid %llu -> %llu, image's oldest xmin %s",
				 (unsigned long long) oldest_before, (unsigned long long) oldest_after, xmin);
		report("horizon", "commit log cut past the image", oldest_after > strtoull(xmin, NULL, 10), d);
	}

	restore(c, &raw);
	{
		int			bad;
		char		d[64];

		expect_failure = true;
		bad = check_table(c, "horizon", raw.sum, false);
		expect_failure = false;
		snprintf(d, sizeof(d), "%d of 6 checks fail", bad);
		report("horizon", "raw image is unreadable or invalid (expected)", bad > 0, d);
	}
	restore(c, &frozen);
	check_table(c, "horizon", frozen.sum, false);
	free(xmin);
	image_free(&raw);
	image_free(&frozen);
}

static void
case_inflight(PGconn *c)
{
	PGconn	   *w = connect_to("xid");
	Image		raw,
				frozen;

	make_table(c);
	qx(w, "BEGIN");
	qx(w, "INSERT INTO t SELECT g, g, md5(g::text) FROM generate_series(20001, 20500) g");
	qx(w, "UPDATE t SET pad = pad || 'x' WHERE id % 17 = 0");	/* HOT */
	qx(w, "UPDATE t SET v = v + 200000 WHERE id % 19 = 0");	/* not HOT */
	qx(w, "DELETE FROM t WHERE id % 23 = 0");
	qx(c, "BEGIN ISOLATION LEVEL REPEATABLE READ");
	capture(c, &raw, false);
	capture(c, &frozen, true);
	qx(c, "COMMIT");
	qx(w, "COMMIT");
	PQfinish(w);

	restore(c, &raw);
	{
		char	   *s = q1(c, sum_sql);

		report("inflight", "raw image shows the later commit (expected)",
			   strcmp(s, raw.sum) != 0, s);
		free(s);
	}
	restore(c, &frozen);
	check_table(c, "inflight", frozen.sum, false);
	image_free(&raw);
	image_free(&frozen);
}

static void
case_multixact(PGconn *c)
{
	PGconn	   *a = connect_to("xid"),
			   *b = connect_to("xid");
	Image		frozen;
	char	   *multis;

	make_table(c);
	qx(a, "BEGIN");
	qx(a, "SELECT * FROM t WHERE id % 5 = 0 FOR SHARE");
	qx(b, "BEGIN");
	qx(b, "SELECT * FROM t WHERE id % 5 = 0 FOR SHARE");
	multis = q1(c, "SELECT count(*) FROM generate_series(0, pg_relation_size('t') / 8192 - 1) b, "
				"heap_page_items(get_raw_page('t', b::int)) WHERE t_infomask & 4096 <> 0");
	report("multixact", "live pages hold multixacts", strcmp(multis, "0") != 0, multis);
	free(multis);
	qx(c, "BEGIN ISOLATION LEVEL REPEATABLE READ");
	capture(c, &frozen, true);
	qx(c, "COMMIT");
	qx(a, "COMMIT");
	qx(b, "COMMIT");
	PQfinish(a);
	PQfinish(b);

	restore(c, &frozen);
	multis = q1(c, "SELECT count(*) FROM generate_series(0, pg_relation_size('t') / 8192 - 1) b, "
				"heap_page_items(get_raw_page('t', b::int)) "
				"WHERE t_infomask & 4096 <> 0 OR (t_xmax IS NOT NULL AND t_xmax <> '0')");
	report("multixact", "frozen image names no multixact or xmax", strcmp(multis, "0") == 0, multis);
	free(multis);
	check_table(c, "multixact", frozen.sum, false);
	image_free(&frozen);
}

static void
case_reuse(PGconn *c)
{
	char	   *before,
			   *after;

	/* the table as the last case left it: restored from a frozen image */
	before = q1(c, "SELECT count(*) FROM t");
	qx(c, "INSERT INTO t SELECT g, g, md5(g::text) FROM generate_series(50001, 51000) g");
	qx(c, "UPDATE t SET pad = pad || 'r' WHERE id % 3 = 0");
	qx(c, "UPDATE t SET v = v + 300000 WHERE id % 5 = 0");
	qx(c, "DELETE FROM t WHERE id % 29 = 0");
	after = q1(c, "SELECT count(*) FROM t");
	report("reuse", "changes after the restore", strcmp(before, after) != 0, after);
	check_table(c, "reuse", NULL, false);
	free(before);
	free(after);
}

int
main(int argc, char **argv)
{
	PGconn	   *c;
	int			ch;
	static struct option opts[] = {{"datadir", 1, 0, 'D'}, {0, 0, 0, 0}};

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		if (ch == 'D')
			datadir = optarg;
	start_cluster();
	c = connect_to("postgres");
	if (PQntuples(PQexec(c, "SELECT 1 FROM pg_database WHERE datname = 'xid'")) == 0)
		qx(c, "CREATE DATABASE xid");
	PQfinish(c);
	c = connect_to("xid");
	qx(c, "CREATE EXTENSION IF NOT EXISTS pageinspect");
	qx(c, "CREATE EXTENSION IF NOT EXISTS amcheck");
	qx(c, "DROP EXTENSION IF EXISTS prolly_pages");
	qx(c, "CREATE EXTENSION prolly_pages");

	case_horizon(c);
	case_inflight(c);
	case_multixact(c);
	case_reuse(c);
	PQfinish(c);
	printf("%s\n", failures ? "xidbench: FAILED" : "xidbench: all passed");
	return failures ? 1 : 0;
}
