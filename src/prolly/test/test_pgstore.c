/*
 * test_pgstore.c
 *	  The history store inside Postgres (bench/pgstore.c), against a running
 *	  server: what is flushed survives reopening, holds and counts are
 *	  rebuilt from the roots, chunks let go are gone from the server too, it
 *	  ends where an in-memory store ends, and remote operations (clone,
 *	  fetch, push, pull) work between it and other stores.
 *
 *	  PROLLY_PG names the database (default: host=/tmp port=5499
 *	  user=postgres dbname=prolly_pgstore, made if missing).  With
 *	  PROLLY_PG_STANDBY naming the same database on a hot standby of that
 *	  server, the store is also opened there.
 */
#define _DEFAULT_SOURCE
#include <libpq-fe.h>
#include <unistd.h>

#include "test_repo.h"
#include "../bench/pgstore.h"

static const char *conninfo;

static ChunkStore *
open_pg(const char *schema, bool fresh)
{
	char		err[512];
	ChunkStore *cs = pgstore_open(conninfo, schema, fresh, false, err, sizeof(err));

	if (cs == NULL)
	{
		fprintf(stderr, "pgstore_open %s: %s\n", schema, err);
		abort();
	}
	return cs;
}

/* The repo saved and flushed, its store closed and opened again, the repo with it. */
static ChunkStore *
reopen(VcRepo *r, ChunkStore *cs, const char *schema)
{
	PHash		st = vc_repo_save(r);

	pgstore_set_state(cs, &st);
	CHECK_EQ(pgstore_flush(cs), 0);
	vc_repo_free(r);
	cs_destroy(cs);
	cs = open_pg(schema, false);
	CHECK(pgstore_get_state(cs, &st));
	CHECK(vc_repo_open(r, cs, &small_policy, &st) == VC_OK);
	return cs;
}

static uint64_t
server_rows(const char *schema)
{
	PGconn	   *c = PQconnectdb(conninfo);
	char		sql[200];
	PGresult   *res;
	uint64_t	n;

	snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s.idx", schema);
	res = PQexec(c, sql);
	CHECK(PQresultStatus(res) == PGRES_TUPLES_OK);
	n = strtoull(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	PQfinish(c);
	return n;
}

static void
same_log(VcRepo *a, VcRepo *b, const PHash *head)
{
	LogAcc		x,
				y;

	CHECK(log_from(a, head, 0, &x) == VC_OK);
	CHECK(log_from(b, head, 0, &y) == VC_OK);
	CHECK_EQ(x.n, y.n);
	for (int i = 0; i < x.n; i++)
		CHECK(phash_eq(&x.v[i], &y.v[i]));
}

/* ------------------------------------------------------------------ tests */

/* Commits, branches and the repo state survive closing and opening the store. */
static void
test_persists_across_reopen(void)
{
	ChunkStore *cs = open_pg("pgs_persist", true);
	VcRepo		r;
	Linear		l;
	PHash		side,
				mainhead;
	LogAcc		acc;
	Snap		sd;

	setup(&r, cs);
	l = build_linear(&r);
	sd = snap(&r.ctx, &l.d);
	CHECK(vc_branch_create(&r, "side", &l.b) == VC_OK);
	CHECK(vc_checkout(&r, "side") == VC_OK);
	side = commit_rows(&r, 50, 90, 7, "side");
	CHECK(vc_checkout(&r, "main") == VC_OK);
	mainhead = vc_head(&r)->head;

	cs = reopen(&r, cs, "pgs_persist");
	CHECK(phash_eq(&vc_head(&r)->head, &mainhead));
	CHECK(phash_eq(&vc_branch_find(&r, "side")->head, &side));
	CHECK(log_from(&r, &mainhead, 0, &acc) == VC_OK);
	CHECK_EQ(acc.n, 5);
	/* the rows come back from the server */
	check_snap(&r.ctx, &l.d, &sd);
	CHECK(pgstore_server_reads(cs) > 0);
	check_store(cs, 0);
	CHECK_EQ(server_rows("pgs_persist"), cs->ops->count(cs));
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * After a flush the store reads what it flushed from the server, in the
 * same session: older commits' rows come back right, and keep coming as
 * more is written and flushed.
 */
static void
test_reads_after_flush(void)
{
	ChunkStore *cs = open_pg("pgs_reads", true);
	VcRepo		r;
	Linear		l;
	Snap		sb,
				sd;
	uint64_t	before;

	setup(&r, cs);
	l = build_linear(&r);
	sb = snap(&r.ctx, &l.b);
	sd = snap(&r.ctx, &l.d);
	CHECK_EQ(pgstore_flush(cs), 0);
	before = pgstore_server_reads(cs);
	check_snap(&r.ctx, &l.b, &sb);
	CHECK(pgstore_server_reads(cs) > before);
	for (int i = 1; i <= 5; i++)
	{
		commit_rows(&r, (uint64_t) i * 300, (uint64_t) i * 300 + 50, (uint64_t) (20 + i), "more");
		CHECK_EQ(pgstore_flush(cs), 0);
		check_snap(&r.ctx, &l.d, &sd);
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* The same work on an in-memory store and on this one ends the same. */
static void
test_same_as_memstore(void)
{
	ChunkStore *mcs = memstore_create(),
			   *pcs = open_pg("pgs_same", true);
	VcRepo		m,
				p;

	setup(&m, mcs);
	setup(&p, pcs);
	rng_seed(31);
	commit_rows(&m, 0, 3000, 0, "load");
	rng_seed(31);
	commit_rows(&p, 0, 3000, 0, "load");
	for (int i = 1; i <= 30; i++)
	{
		commit_rows(&m, (uint64_t) i * 50, (uint64_t) i * 50 + 40, (uint64_t) i, "step");
		commit_rows(&p, (uint64_t) i * 50, (uint64_t) i * 50 + 40, (uint64_t) i, "step");
		if (i % 10 == 0)
		{
			/* the same steps on both: saved, closed and opened again */
			PHash		st = vc_repo_save(&m);

			vc_repo_free(&m);
			CHECK(vc_repo_open(&m, mcs, &small_policy, &st) == VC_OK);
			pcs = reopen(&p, pcs, "pgs_same");
		}
	}
	/*
	 * Each repo's history is its own (its first commit is unique), and so
	 * are the trees its state keeps by commit; the rows are the same, chunk
	 * for chunk.
	 */
	CHECK(phash_eq(&vc_head(&m)->head_root, &vc_head(&p)->head_root));
	{
		PHashMap	a,
					b;

		phmap_init(&a);
		phmap_init(&b);
		closure(mcs, &vc_head(&m)->head_root, &a);
		closure(pcs, &vc_head(&p)->head_root, &b);
		CHECK(set_size(&a) > 10);
		CHECK(set_equal(&a, &b));
		phmap_free(&a);
		phmap_free(&b);
	}
	check_store(mcs, 0);
	check_store(pcs, 0);
	vc_repo_free(&m);
	vc_repo_free(&p);
	cs_destroy(mcs);
	cs_destroy(pcs);
}

/* History severed and discarded leaves the server's index too, and stays gone. */
static void
test_discarded_history_leaves_the_server(void)
{
	ChunkStore *cs = open_pg("pgs_discard", true);
	VcRepo		r;
	Linear		l;
	uint32_t	seg;
	uint64_t	before;

	setup(&r, cs);
	l = build_linear(&r);
	cs = reopen(&r, cs, "pgs_discard");
	before = server_rows("pgs_discard");
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_discard(&r, seg) == VC_OK);
	cs_collect_orphans(cs);
	CHECK(!cs_has(cs, &l.a));
	cs = reopen(&r, cs, "pgs_discard");
	CHECK(!cs_has(cs, &l.a) && !cs_has(cs, &l.init));
	CHECK(cs_has(cs, &l.d));
	CHECK(server_rows("pgs_discard") < before);
	CHECK_EQ(server_rows("pgs_discard"), cs->ops->count(cs));
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Remotes across stores: a laptop on memory clones an origin in Postgres,
 * commits and pushes; a second Postgres store clones the origin, fetches
 * and pulls from the laptop; each store, reopened, has what it was sent.
 */
static void
test_remotes_across_stores(void)
{
	ChunkStore *ocs = open_pg("pgs_origin", true),
			   *lcs = memstore_create(),
			   *bcs;
	VcRepo		origin,
				laptop,
				b;
	PHash		pushed,
				fetched,
				head;
	VcMergeResult mr;

	setup(&origin, ocs);
	vc_repo_set_name(&origin, "origin");
	build_linear(&origin);
	ocs = reopen(&origin, ocs, "pgs_origin");

	/* a clone from Postgres into memory, a push back */
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	vc_repo_set_name(&laptop, "laptop");
	pushed = commit_rows(&laptop, 700, 800, 11, "from the laptop");
	CHECK(vc_push(&laptop, &origin, "main") == VC_OK);
	ocs = reopen(&origin, ocs, "pgs_origin");
	vc_repo_set_name(&origin, "origin");
	CHECK(phash_eq(&vc_head(&origin)->head, &pushed));
	same_log(&origin, &laptop, &pushed);
	check_store(ocs, 0);

	/* a clone from Postgres into Postgres */
	bcs = open_pg("pgs_b", true);
	CHECK(vc_repo_clone(&b, bcs, &origin, 0) == VC_OK);
	vc_repo_set_name(&b, "b");
	bcs = reopen(&b, bcs, "pgs_b");
	vc_repo_set_name(&b, "b");
	CHECK(phash_eq(&vc_head(&b)->head, &pushed));

	/* a fetch from memory into Postgres */
	fetched = commit_rows(&laptop, 900, 950, 12, "more from the laptop");
	CHECK(vc_fetch(&b, &laptop, "main", &head) == VC_OK);
	CHECK(phash_eq(&head, &fetched));
	bcs = reopen(&b, bcs, "pgs_b");
	vc_repo_set_name(&b, "b");
	CHECK(cs_has(bcs, &fetched));
	same_log(&b, &laptop, &fetched);

	/* a pull that merges: b has its own commit, the laptop another */
	commit_rows(&b, 2000, 2100, 13, "b's own");
	commit_rows(&laptop, 3000, 3100, 14, "the laptop's own");
	CHECK(vc_pull(&b, &laptop, "main", 0, &mr) == VC_OK);
	CHECK(mr.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&mr);
	head = vc_head(&b)->head;
	bcs = reopen(&b, bcs, "pgs_b");
	CHECK(phash_eq(&vc_head(&b)->head, &head));
	check_store(bcs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	vc_repo_free(&b);
	cs_destroy(ocs);
	cs_destroy(lcs);
	cs_destroy(bcs);
}

/* A store made empty again drops what it had; an ephemeral one leaves nothing. */
static void
test_fresh_and_ephemeral(void)
{
	char		err[256];
	ChunkStore *cs = open_pg("pgs_fresh", true);
	VcRepo		r;
	PGconn	   *c;
	PGresult   *res;

	setup(&r, cs);
	build_linear(&r);
	cs = reopen(&r, cs, "pgs_fresh");
	CHECK(server_rows("pgs_fresh") > 0);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs = open_pg("pgs_fresh", true);
	CHECK_EQ(cs->ops->count(cs), 0);
	CHECK_EQ(server_rows("pgs_fresh"), 0);
	cs_destroy(cs);

	cs = pgstore_open(conninfo, "pgs_gone", true, true, err, sizeof(err));
	CHECK(cs != NULL);
	cs_destroy(cs);
	c = PQconnectdb(conninfo);
	res = PQexec(c, "SELECT count(*) FROM pg_namespace WHERE nspname = 'pgs_gone'");
	CHECK_EQ(strtoull(PQgetvalue(res, 0, 0), NULL, 10), 0);
	PQclear(res);
	PQfinish(c);
}

/* Waits until the standby has replayed everything the primary has written. */
static void
await_replay(const char *standby)
{
	PGconn	   *p = PQconnectdb(conninfo),
			   *s = PQconnectdb(standby);
	PGresult   *res = PQexec(p, "SELECT pg_current_wal_lsn()");
	char		sql[200];

	snprintf(sql, sizeof(sql), "SELECT pg_last_wal_replay_lsn() >= '%s'", PQgetvalue(res, 0, 0));
	PQclear(res);
	for (int i = 0; i < 300; i++)
	{
		bool		done;

		res = PQexec(s, sql);
		done = PQresultStatus(res) == PGRES_TUPLES_OK && PQgetvalue(res, 0, 0)[0] == 't';
		PQclear(res);
		if (done)
			break;
		usleep(100000);
	}
	PQfinish(p);
	PQfinish(s);
}

/*
 * A hot standby has the store as the primary has it, with nothing of ours
 * to ship it: flushed history opens there, read only, and follows as the
 * primary goes on.
 */
static void
test_on_a_standby(const char *standby)
{
	ChunkStore *cs = open_pg("pgs_standby", true),
			   *scs;
	VcRepo		r,
				s;
	Linear		l;
	Snap		sd;
	PHash		st,
				later;
	char		err[512];

	setup(&r, cs);
	l = build_linear(&r);
	sd = snap(&r.ctx, &l.d);
	st = vc_repo_save(&r);
	pgstore_set_state(cs, &st);
	CHECK_EQ(pgstore_flush(cs), 0);
	await_replay(standby);

	scs = pgstore_open(standby, "pgs_standby", false, false, err, sizeof(err));
	if (scs == NULL)
		fprintf(stderr, "standby: %s\n", err);
	CHECK(scs != NULL);
	CHECK(pgstore_get_state(scs, &st));
	CHECK(vc_repo_open(&s, scs, &small_policy, &st) == VC_OK);
	CHECK(phash_eq(&vc_head(&s)->head, &l.d));
	check_snap(&s.ctx, &l.d, &sd);
	same_log(&r, &s, &l.d);
	/* read only there: a flush is refused */
	CHECK(pgstore_flush(scs) != 0);
	vc_repo_free(&s);
	cs_destroy(scs);

	/* the primary goes on; the standby follows */
	later = commit_rows(&r, 4000, 4100, 30, "later");
	st = vc_repo_save(&r);
	pgstore_set_state(cs, &st);
	CHECK_EQ(pgstore_flush(cs), 0);
	await_replay(standby);
	scs = pgstore_open(standby, "pgs_standby", false, false, err, sizeof(err));
	CHECK(scs != NULL);
	CHECK(pgstore_get_state(scs, &st));
	CHECK(vc_repo_open(&s, scs, &small_policy, &st) == VC_OK);
	CHECK(phash_eq(&vc_head(&s)->head, &later));
	same_log(&r, &s, &later);
	vc_repo_free(&s);
	cs_destroy(scs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

int
main(void)
{
	conninfo = getenv("PROLLY_PG");
	if (conninfo == NULL)
	{
		PGconn	   *c = PQconnectdb("host=/tmp port=5499 user=postgres dbname=postgres");
		PGresult   *res;

		if (PQstatus(c) != CONNECTION_OK)
		{
			fprintf(stderr, "test_pgstore: no server: %s", PQerrorMessage(c));
			return 1;
		}
		res = PQexec(c, "SELECT 1 FROM pg_database WHERE datname = 'prolly_pgstore'");
		if (PQntuples(res) == 0)
			PQclear(PQexec(c, "CREATE DATABASE prolly_pgstore"));
		PQclear(res);
		PQfinish(c);
		conninfo = "host=/tmp port=5499 user=postgres dbname=prolly_pgstore";
	}
	RUN(test_persists_across_reopen);
	RUN(test_reads_after_flush);
	RUN(test_same_as_memstore);
	RUN(test_discarded_history_leaves_the_server);
	RUN(test_remotes_across_stores);
	RUN(test_fresh_and_ephemeral);
	if (getenv("PROLLY_PG_STANDBY"))
	{
		const char *standby = getenv("PROLLY_PG_STANDBY");

		printf("%-44s", "test_on_a_standby");
		fflush(stdout);
		test_on_a_standby(standby);
		printf("ok\n");
	}
	printf("test_pgstore: all tests passed\n");
	return 0;
}
