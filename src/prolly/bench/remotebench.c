/*
 * remotebench.c
 *	  Clone, push, fetch and pull against a plain-storage remote (a directory,
 *	  or an object server over HTTP: bench/objserver), measured: where the
 *	  time goes, how many requests, how many bytes, and how each grows with
 *	  the size of the database and of the change.
 *
 *	remotebench --remote URL|DIR --work DIR [--rows N] [--row-bytes B]
 *	            [--history H] [--deltas 1,100,10000] [--parallel P]
 *
 * The origin repo is built locally (N rows in one table, then H commits of
 * changes), pushed to the empty remote (the remote made by cloning into
 * it), then cloned back three ways: a chunk at a time (the transfer as it
 * is), a level of the tree at a time (each level's missing chunks read in
 * one batch of parallel ranged reads), and by streaming the remote's packs
 * whole.  Then, for each change size, the origin changes that many rows and
 * pushes; the clone fetches; and the clone, with a change of its own, pulls
 * (a fetch and a merge).  Local stores are directory stores (the journal
 * store), one per repo under --work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../chunkstore.h"
#include "../prolly.h"
#include "../prolly_util.h"
#include "../vc.h"
#include "packstore.h"
#include "pgstore.h"
#include <libpq-fe.h>
#include "remote.h"

static const char *remote_arg;
static const char *work = "/tmp/remotebench";
static uint64_t nrows = 100000;
static size_t row_bytes = 100;
static int	nhistory = 10;
static int	parallel = 16;
static uint64_t deltas[16] = {1, 100, 10000};
static int	ndeltas = 3;
static int	seq = 0;
static bool skip_per_chunk_clone = false;
static const char *pg_conninfo = NULL;	/* local stores in the page store */
static PGconn *walconn = NULL;

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v text)";

static double
now_secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
die(const char *what)
{
	fprintf(stderr, "remotebench: %s\n", what);
	exit(1);
}

static Remote *
open_remote(void)
{
	if (strncmp(remote_arg, "http://", 7) == 0)
		return remote_http_open(remote_arg, parallel);
	return remote_dir_open(remote_arg);
}

static ChunkStore *
local_store(const char *name)
{
	char		dir[1024],
				err[256];
	ChunkStore *cs;

	if (pg_conninfo)
	{
		snprintf(dir, sizeof(dir), "rb_%s_%d_%d", name, (int) getpid(), seq++);
		cs = pgstore_open(pg_conninfo, dir, true, false, err, sizeof(err));
		if (cs == NULL)
			die(err);
		return cs;
	}
	snprintf(dir, sizeof(dir), "%s/%s-%d", work, name, seq++);
	mkdir(work, 0755);
	cs = dirstore_open(dir, err, sizeof(err));
	if (cs == NULL)
		die(err);
	return cs;
}

static void
local_flush(ChunkStore *cs)
{
	if (pg_conninfo)
		pgstore_flush(cs);
	else
		dirstore_flush(cs);
}

/* The page store's server's WAL position (0 if local stores are directories). */
static uint64_t
wal_pos(void)
{
	PGresult   *res;
	uint64_t	v = 0;

	if (walconn == NULL)
		return 0;
	res = PQexec(walconn, "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '0/0')::int8");
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
		v = strtoull(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	return v;
}

/* ------------------------------------------------------------- the data */

static uint64_t rng = 88172645463325252ULL;

static uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* Puts rows [keys] with values of row_bytes, made of 'ver'. */
static void
edit_rows(VcRepo *r, const uint64_t *keys, size_t n, uint64_t ver)
{
	ProllyEdit *e = px_malloc(n * sizeof(ProllyEdit));
	uint8_t    *kb = px_malloc(n * 8);
	uint8_t    *vb = px_malloc(n * row_bytes);
	PHash		rv = vc_working(r),
				out;

	for (size_t i = 0; i < n; i++)
	{
		uint64_t	x = keys[i] * 0x9E3779B97F4A7C15ULL ^ ver;

		for (int b = 0; b < 8; b++)
			kb[i * 8 + b] = (uint8_t) (keys[i] >> (56 - 8 * b));
		for (size_t b = 0; b < row_bytes; b++)
		{
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			vb[i * row_bytes + b] = (uint8_t) ('a' + x % 26);
		}
		e[i] = (ProllyEdit) {kb + i * 8, 8, vb + i * row_bytes, row_bytes, false};
	}
	if (vc_table_edit(&r->ctx, &rv, "t", e, n, &out) != 0)
		die("could not edit the table");
	vc_set_working(r, &out);
	free(e);
	free(kb);
	free(vb);
}

static int
u64_cmp(const void *a, const void *b)
{
	uint64_t	x = *(const uint64_t *) a,
				y = *(const uint64_t *) b;

	return x < y ? -1 : x > y;
}

/* A commit changing n random rows. */
static PHash
change_rows(VcRepo *r, uint64_t n, uint64_t ver, const char *msg)
{
	uint64_t   *keys = px_malloc(n * sizeof(uint64_t));
	size_t		k = 0;

	for (uint64_t i = 0; i < n; i++)
		keys[i] = rnd() % nrows;
	qsort(keys, n, sizeof(uint64_t), u64_cmp);
	for (uint64_t i = 0; i < n; i++)
		if (k == 0 || keys[k - 1] != keys[i])
			keys[k++] = keys[i];
	edit_rows(r, keys, k, ver);
	free(keys);
	return vc_commit(r, msg);
}

/* ---------------------------------------------------------- measuring */

typedef struct Snap
{
	double		t;
	RemoteStats rs;
	ChunkStoreStats ls;
	uint64_t	wal;
} Snap;

static Snap
snap(Remote *rm, ChunkStore *local)
{
	Snap		s;

	s.t = now_secs();
	s.rs = *remote_stats(rm);
	s.ls = local ? local->stats : (ChunkStoreStats) {0};
	s.wal = wal_pos();
	return s;
}

static void
report(const char *op, const char *how, Remote *rm, ChunkStore *local,
	   ChunkStore *ps, Snap a, double open_secs)
{
	Snap		b = snap(rm, local);
	PackStoreStats *pst = ps ? packstore_stats(ps) : NULL;

	printf("%-8s %-12s %8.3f s  open %7.3f s  remote %7.3f s  req %7llu  "
		   "up %10llu B  down %10llu B  new %7llu chunks %10llu B  ranges %7llu  idx %9llu B  wal %10llu B\n",
		   op, how, b.t - a.t, open_secs, b.rs.secs - a.rs.secs,
		   (unsigned long long) (b.rs.requests - a.rs.requests),
		   (unsigned long long) (b.rs.bytes_up - a.rs.bytes_up),
		   (unsigned long long) (b.rs.bytes_down - a.rs.bytes_down),
		   (unsigned long long) (b.ls.new_chunks - a.ls.new_chunks),
		   (unsigned long long) (b.ls.new_bytes - a.ls.new_bytes),
		   (unsigned long long) (pst ? pst->range_gets : 0),
		   (unsigned long long) (pst ? pst->index_bytes : 0),
		   (unsigned long long) (b.wal - a.wal));
	fflush(stdout);
}

/* Opens the remote's store and repo. */
static ChunkStore *
open_remote_repo(Remote *rm, VcRepo *rr, double *secs)
{
	char		err[256];
	PHash		state;
	ChunkStore *ps;
	double		t0 = now_secs();
	static const PHash zero;

	ps = packstore_open(rm, &state, err, sizeof(err));
	if (ps == NULL)
		die(err);
	if (memcmp(&state, &zero, sizeof(zero)) == 0)
		die("the remote has no repo");
	if (vc_repo_open(rr, ps, &prolly_default_policy, &state) != VC_OK)
		die("could not open the remote's repo");
	vc_repo_set_name(rr, "origin");
	*secs = now_secs() - t0;
	return ps;
}

static void
save_remote(VcRepo *rr, ChunkStore *ps)
{
	PHash		st = vc_repo_save(rr);

	if (packstore_flush(ps, &st) != 0)
		die("could not swap the remote's manifest");
}

/* ------------------------------------------------------------ the runs */

typedef struct Ingest
{
	ChunkStore *cs;
	uint64_t	n;
} Ingest;

static void
ingest_fn(void *arg, const PHash *h, const uint8_t *data, size_t len)
{
	Ingest	   *in = arg;
	PHash		got,
				err;

	if (cs_put(in->cs, data, len, &got, &err) != CS_OK || !phash_eq(&got, h))
		die("a streamed chunk did not store");
	in->n++;
}

static void
clone_back(const char *how, VcRepo *out, ChunkStore **outcs)
{
	Remote	   *rm = open_remote();
	ChunkStore *lcs = local_store("clone");
	VcRepo		rr;
	double		open_secs;
	Snap		a = snap(rm, lcs);
	ChunkStore *ps = open_remote_repo(rm, &rr, &open_secs);

	if (strcmp(how, "per-chunk") == 0)
		cs_batched_copy = false;
	else if (strcmp(how, "levels") == 0)
		cs_batched_copy = true;
	else
	{
		Ingest		in = {lcs, 0};

		if (!packstore_stream(ps, ingest_fn, &in))
			die("could not stream the remote's packs");
		cs_batched_copy = false;
	}
	if (vc_repo_clone(out, lcs, &rr, 0) != VC_OK)
		die("clone failed");
	vc_repo_set_name(out, "laptop");
	local_flush(lcs);
	report("clone", how, rm, lcs, ps, a, open_secs);
	vc_repo_free(&rr);
	cs_destroy(ps);
	remote_close(rm);
	*outcs = lcs;
}

int
main(int argc, char **argv)
{
	VcRepo		origin;
	ChunkStore *ocs;
	PHash		rv,
				out;
	double		t0;

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc)
			remote_arg = argv[++i];
		else if (strcmp(argv[i], "--work") == 0 && i + 1 < argc)
			work = argv[++i];
		else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
			nrows = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--row-bytes") == 0 && i + 1 < argc)
			row_bytes = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--history") == 0 && i + 1 < argc)
			nhistory = atoi(argv[++i]);
		else if (strcmp(argv[i], "--local-pg") == 0 && i + 1 < argc)
			pg_conninfo = argv[++i];
		else if (strcmp(argv[i], "--skip-per-chunk-clone") == 0)
			skip_per_chunk_clone = true;
		else if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc)
			parallel = atoi(argv[++i]);
		else if (strcmp(argv[i], "--deltas") == 0 && i + 1 < argc)
		{
			char	   *s = argv[++i],
					   *tok,
					   *save;

			ndeltas = 0;
			for (tok = strtok_r(s, ",", &save); tok && ndeltas < 16; tok = strtok_r(NULL, ",", &save))
				deltas[ndeltas++] = strtoull(tok, NULL, 10);
		}
		else
			die("usage: remotebench --remote URL|DIR --work DIR [--rows N] [--row-bytes B] [--history H] [--deltas a,b,c] [--parallel P]");
	}
	if (remote_arg == NULL)
		die("--remote is required");
	if (pg_conninfo)
	{
		walconn = PQconnectdb(pg_conninfo);
		if (PQstatus(walconn) != CONNECTION_OK)
			die("could not connect to the page store's server");
	}

	/* the origin: N rows, then H commits of changes */
	t0 = now_secs();
	ocs = local_store("origin");
	vc_repo_init(&origin, ocs, &prolly_default_policy);
	vc_repo_set_name(&origin, "origin-local");
	rv = vc_working(&origin);
	if (vc_table_create(&origin.ctx, &rv, "t", ddl, strlen(ddl), &out) != 0)
		die("could not create the table");
	vc_set_working(&origin, &out);
	for (uint64_t lo = 0; lo < nrows; lo += 100000)
	{
		uint64_t	hi = lo + 100000 < nrows ? lo + 100000 : nrows;
		uint64_t   *keys = px_malloc((hi - lo) * sizeof(uint64_t));

		for (uint64_t k = lo; k < hi; k++)
			keys[k - lo] = k;
		edit_rows(&origin, keys, hi - lo, 0);
		free(keys);
	}
	vc_commit(&origin, "load");
	for (int i = 0; i < nhistory; i++)
		change_rows(&origin, nrows / 100 > 0 ? nrows / 100 : 1, (uint64_t) i + 1, "history");
	local_flush(ocs);
	printf("# %llu rows of %zu bytes, %d history commits; origin store %zu chunks, %llu bytes, built in %.1f s\n",
		   (unsigned long long) nrows, row_bytes, nhistory, ocs->ops->count(ocs),
		   (unsigned long long) ocs->ops->bytes(ocs), now_secs() - t0);

	/* the first push: the remote made by cloning into it */
	{
		Remote	   *rm = open_remote();
		char		err[256];
		PHash		state;
		Snap		a = snap(rm, NULL);
		ChunkStore *ps = packstore_open(rm, &state, err, sizeof(err));
		VcRepo		rr;

		printf("# remote %s\n", remote_describe(rm));
		if (ps == NULL)
			die(err);
		if (vc_repo_clone(&rr, ps, &origin, 0) != VC_OK)
			die("first push failed");
		save_remote(&rr, ps);
		report("push", "first", rm, NULL, ps, a, packstore_stats(ps)->open_secs);
		vc_repo_free(&rr);
		cs_destroy(ps);
		remote_close(rm);
	}

	/* clones, three ways */
	{
		VcRepo		c1,
					c2,
					c3;
		ChunkStore *cs1,
				   *cs2,
				   *cs3;

		if (!skip_per_chunk_clone)
		{
			clone_back("per-chunk", &c1, &cs1);
			vc_repo_free(&c1);
			cs_destroy(cs1);
		}
		clone_back("levels", &c2, &cs2);
		clone_back("stream", &c3, &cs3);
		vc_repo_free(&c2);
		cs_destroy(cs2);

		/* changes: push from the origin, fetch and pull into the clone */
		for (int d = 0; d < ndeltas; d++)
		{
			for (int mode = 0; mode < 2; mode++)
			{
				const char *how = mode == 0 ? "per-chunk" : "levels";
				char		tag[32];

				cs_batched_copy = mode == 1;
				snprintf(tag, sizeof(tag), "%s/%llu", how, (unsigned long long) deltas[d]);

				change_rows(&origin, deltas[d], 1000 + (uint64_t) d * 2 + mode, "change");
				{
					Remote	   *rm = open_remote();
					VcRepo		rr;
					double		os;
					Snap		a = snap(rm, NULL);
					ChunkStore *ps = open_remote_repo(rm, &rr, &os);

					if (vc_push(&origin, &rr, "main") != VC_OK)
						die("push failed");
					save_remote(&rr, ps);
					report("push", tag, rm, NULL, ps, a, os);
					vc_repo_free(&rr);
					cs_destroy(ps);
					remote_close(rm);
				}
				{
					Remote	   *rm = open_remote();
					VcRepo		rr;
					double		os;
					PHash		head;
					Snap		a = snap(rm, cs3);
					ChunkStore *ps = open_remote_repo(rm, &rr, &os);

					if (vc_fetch(&c3, &rr, "main", &head) != VC_OK)
						die("fetch failed");
					if (!phash_eq(&head, &vc_head(&origin)->head))
						die("the fetched head is not the origin's");
					local_flush(cs3);
					report("fetch", tag, rm, cs3, ps, a, os);
					vc_repo_free(&rr);
					cs_destroy(ps);
					remote_close(rm);
				}
				/* the origin changes again; the clone, with its own change, pulls */
				change_rows(&origin, deltas[d], 2000 + (uint64_t) d * 2 + mode, "change");
				{
					Remote	   *rm = open_remote();
					VcRepo		rr;
					double		os;
					ChunkStore *ps = open_remote_repo(rm, &rr, &os);

					if (vc_push(&origin, &rr, "main") != VC_OK)
						die("push failed");
					save_remote(&rr, ps);
					vc_repo_free(&rr);
					cs_destroy(ps);
					remote_close(rm);
				}
				change_rows(&c3, deltas[d], 3000 + (uint64_t) d * 2 + mode, "local");
				{
					Remote	   *rm = open_remote();
					VcRepo		rr;
					double		os;
					VcMergeResult res;
					Snap		a = snap(rm, cs3);
					ChunkStore *ps = open_remote_repo(rm, &rr, &os);
					int			rc = vc_pull(&c3, &rr, "main", 0, &res);

					if (rc != VC_OK)
						die("pull failed");
					local_flush(cs3);
					report("pull", tag, rm, cs3, ps, a, os);
					vc_repo_free(&rr);
					cs_destroy(ps);
					remote_close(rm);
				}
			}
		}
		vc_repo_free(&c3);
		cs_destroy(cs3);
	}
	vc_repo_free(&origin);
	cs_destroy(ocs);
	return 0;
}
