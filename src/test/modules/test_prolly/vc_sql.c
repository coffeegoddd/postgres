/*-------------------------------------------------------------------------
 *
 * vc_sql.c
 *	  SQL functions driving version control on this database's history
 *	  store and its references (pg_history.pointers), for tests of many
 *	  backends at once: commits, branches, resets, and what they left.
 *
 * Each call views the repository afresh (prolly/engine/vc.h), on the
 * branch it names, with the engine's chunk formats for the call.  A
 * commit's rows are one table, "t", made as the first commit needs it:
 * a commit writes one row, keyed by its message's hash, so that its
 * change is its own.  Commit hashes follow the clock; the tests compare
 * messages and booleans, not hashes.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/vc_sql.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "prolly/engine/chunkfmt.h"
#include "prolly/engine/vc.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "test_prolly.h"

PG_FUNCTION_INFO_V1(test_vc_open);
PG_FUNCTION_INFO_V1(test_vc_commit);
PG_FUNCTION_INFO_V1(test_vc_refs_count);
PG_FUNCTION_INFO_V1(test_vc_round_trip);
PG_FUNCTION_INFO_V1(test_vc_branch);
PG_FUNCTION_INFO_V1(test_vc_branch_delete);
PG_FUNCTION_INFO_V1(test_vc_reset);
PG_FUNCTION_INFO_V1(test_vc_log);
PG_FUNCTION_INFO_V1(test_vc_working_has);
PG_FUNCTION_INFO_V1(test_vc_head_in_window);
PG_FUNCTION_INFO_V1(test_vc_clear);
PG_FUNCTION_INFO_V1(test_vc_rows);
PG_FUNCTION_INFO_V1(test_vc_sever);
PG_FUNCTION_INFO_V1(test_vc_branch_at);
PG_FUNCTION_INFO_V1(test_vc_merge);
PG_FUNCTION_INFO_V1(test_vc_rewrite);
PG_FUNCTION_INFO_V1(test_vc_cancel);
PG_FUNCTION_INFO_V1(test_vc_cancel_writes);
PG_FUNCTION_INFO_V1(test_vc_transfer);
PG_FUNCTION_INFO_V1(test_vc_bench_commits);
PG_FUNCTION_INFO_V1(test_vc_bench_branches);
PG_FUNCTION_INFO_V1(test_vc_bench_log);
PG_FUNCTION_INFO_V1(test_vc_bench_diff);
PG_FUNCTION_INFO_V1(test_vc_bench_load);
PG_FUNCTION_INFO_V1(test_vc_bench_edit);
PG_FUNCTION_INFO_V1(test_vc_bench_merge);
PG_FUNCTION_INFO_V1(test_backend_private_mb);
PG_FUNCTION_INFO_V1(test_vc_bench_sever);
PG_FUNCTION_INFO_V1(test_vc_bench_deep_probe);
PG_FUNCTION_INFO_V1(test_vc_bench_deep_commits);

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

/* a fixed clock, for opening empty repositories in one microsecond */
static uint64 fixed_time;

static uint64
fixed_clock(void *arg)
{
	return fixed_time;
}

static const ChunkFormatRoutine *saved_format;

/*
 * The repository viewed, the engine's formats set (VC_CALL puts them
 * back); with init_time, an empty one's init commit made at that time.
 */
static void
vc_begin(VcRepo *r, int64 init_time)
{
	ChunkStore *cs = ChunkStoreOpen("pages");

	saved_format = ChunkFormat;
	ChunkFormat = &EngineChunkFormat;
	PG_TRY();
	{
		fixed_time = (uint64) init_time;
		vc_repo_open_clock(r, cs, VcRefsOpen("pointers", cs), &prolly_default_policy,
						   init_time > 0 ? fixed_clock : NULL, NULL);
	}
	PG_CATCH();
	{
		ChunkFormat = saved_format;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/* Runs body on the repository, the formats put back however it ends. */
#define VC_CALL(r, init_time, ...) \
	do { \
		vc_begin(&(r), (init_time)); \
		PG_TRY(); \
		{ \
			__VA_ARGS__; \
		} \
		PG_FINALLY(); \
		{ \
			ChunkFormat = saved_format; \
		} \
		PG_END_TRY(); \
		vc_repo_free(&(r)); \
	} while (0)

/* (a tree call that must succeed) */
static void
vc_must_ok(int rc, ProllyCtx *ctx)
{
	if (rc < 0)
		prolly_report(ctx);
}

/* (a commit's row: its message's hash its key) */
static uint64
msg_key(const char *msg)
{
	return (uint64) hash_bytes((const unsigned char *) msg, (int) strlen(msg));
}

static void
checkout(VcRepo *r, const char *branch)
{
	if (vc_checkout(r, branch) != VC_OK)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no branch \"%s\"", branch)));
}

/* test_vc_open(init_time int8) returns bool: the repository, made if new */
Datum
test_vc_open(PG_FUNCTION_ARGS)
{
	VcRepo		r;

	VC_CALL(r, PG_GETARG_INT64(0), (void) 0);
	PG_RETURN_BOOL(true);
}

/* A row written on the branch's working set and committed, msg its message. */
static PHash
commit_msg(VcRepo *r, const char *branch, const char *msg)
{
	PHash		rv,
				out;
	VcTable		t;
	uint8		key[8],
				val[16];
	ProllyEdit	e = {key, 8, val, 16, false};

	checkout(r, branch);
	/* (an edit: locked as it reads, so a second on the branch waits) */
	rv = vc_working_lock(r);
	if (!vc_table_get(&r->ctx, &rv, "t", &t))
	{
		if (vc_table_create(&r->ctx, &rv, "t", ddl, strlen(ddl), &rv) != VC_OK)
			elog(ERROR, "could not make table t");
	}
	put_u64(key, msg_key(msg));
	put_u64(val, msg_key(msg));
	put_u64(val + 8, 1);
	if (vc_table_edit(&r->ctx, &rv, "t", &e, 1, &out) != VC_OK)
		elog(ERROR, "could not write table t");
	vc_set_working(r, &out);
	return vc_commit(r, msg);
}

/*
 * test_vc_commit(branch text, msg text) returns bool: a row written on the
 * branch's working set and committed
 */
Datum
test_vc_commit(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *msg = text_to_cstring(PG_GETARG_TEXT_PP(1));
	VcRepo		r;

	VC_CALL(r, 0,
	{
		(void) commit_msg(&r, branch, msg);
	});
	PG_RETURN_BOOL(true);
}

/* test_vc_branch(name text, from text) returns bool: made at from's head */
Datum
test_vc_branch(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *from = text_to_cstring(PG_GETARG_TEXT_PP(1));
	VcRepo		r;
	bool		made = false;

	VC_CALL(r, 0,
	{
		checkout(&r, from);
		made = vc_branch_create(&r, name, NULL) == VC_OK;
	});
	PG_RETURN_BOOL(made);
}

/* test_vc_branch_delete(name text) returns bool */
Datum
test_vc_branch_delete(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	VcRepo		r;
	bool		deleted = false;

	VC_CALL(r, 0, deleted = vc_branch_delete(&r, name) == VC_OK);
	PG_RETURN_BOOL(deleted);
}

/* (the newest commit on the branch's first-parent line with msg) */
typedef struct FindArg
{
	const char *msg;
	bool		found;
	PHash		hash;
} FindArg;

static void
find_fn(void *arg, const VcCommit *c)
{
	FindArg    *f = arg;

	if (!f->found && c->msglen == strlen(f->msg) &&
		memcmp(c->msg, f->msg, c->msglen) == 0)
	{
		f->found = true;
		f->hash = c->hash;
	}
}

/*
 * test_vc_reset(branch text, to_msg text, hard bool) returns bool: the
 * branch reset to the commit on its line with message to_msg, found as
 * the call begins (before the reset locks the branch)
 */
Datum
test_vc_reset(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *to = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		hard = PG_GETARG_BOOL(2);
	VcRepo		r;
	bool		done = false;

	VC_CALL(r, 0,
	{
		VcBranch	b;
		FindArg		f = {to, false};

		checkout(&r, branch);
		vc_head(&r, &b);
		if (vc_log(&r, &b.head, 1000, 0, find_fn, &f) != VC_OK || !f.found)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" has no commit \"%s\"", branch, to)));
		vc_branch_free(&b);
		done = (hard ? vc_reset_hard(&r, &f.hash) :
				vc_reset_soft(&r, &f.hash)) == VC_OK;
	});
	PG_RETURN_BOOL(done);
}

static void
log_fn(void *arg, const VcCommit *c)
{
	StringInfo	s = arg;

	if (s->len > 0)
		appendStringInfoChar(s, ' ');
	appendBinaryStringInfo(s, c->msg, (int) c->msglen);
}

/*
 * test_vc_log(branch text) returns text: the messages of its first-parent
 * line, newest first; NULL if there is no such branch
 */
Datum
test_vc_log(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	VcRepo		r;
	StringInfoData s;
	bool		found = false;

	initStringInfo(&s);
	VC_CALL(r, 0,
	{
		VcBranch	b;

		if (vc_branch_get(&r, branch, &b))
		{
			found = true;
			if (vc_log(&r, &b.head, 100000, VC_WINDOW_ONLY, log_fn, &s) != VC_OK)
				elog(ERROR, "the log of \"%s\" stops before its root", branch);
			vc_branch_free(&b);
		}
	});
	if (!found)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(s.data));
}

/*
 * test_vc_working_has(branch text, msg text) returns bool: whether the
 * branch's working set has the row a commit with message msg writes
 */
Datum
test_vc_working_has(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *msg = text_to_cstring(PG_GETARG_TEXT_PP(1));
	VcRepo		r;
	bool		has = false;

	VC_CALL(r, 0,
	{
		PHash		rv;
		VcTable		t;
		uint8		key[8];
		const uint8 *v;
		size_t		vl;

		checkout(&r, branch);
		rv = vc_working(&r);
		put_u64(key, msg_key(msg));
		if (vc_table_get(&r.ctx, &rv, "t", &t) &&
			prolly_get(&r.ctx, &t.data, key, 8, &v, &vl) == PROLLY_OK)
		{
			has = true;
			px_free((void *) v);
		}
	});
	PG_RETURN_BOOL(has);
}

/* test_vc_head_in_window(branch text) returns bool: its head has its row */
Datum
test_vc_head_in_window(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	VcRepo		r;
	bool		in = false;

	VC_CALL(r, 0,
	{
		VcBranch	b;

		if (vc_branch_get(&r, branch, &b))
		{
			in = vc_in_window(&r, &b.head);
			vc_branch_free(&b);
		}
	});
	PG_RETURN_BOOL(in);
}

typedef struct NameList
{
	List	   *names;
} NameList;

static void
collect_fn(void *arg, const char *name, const PHash *hash)
{
	NameList   *l = arg;

	l->names = lappend(l->names, pstrdup(name));
}

static void
no_ref_fn(void *arg, const PHash *h, bool strong)
{
}

/* (an unheld chunk not of the engine's formats: another test's) */
static void
engine_only_fn(void *arg, const uint8 *hash, uint32 len, const ChunkCounts *counts)
{
	ChunkStore *cs = arg;
	uint32		l;
	uint8	   *d;

	if (counts->refs != 0 || counts->roots != 0)
		return;
	d = cs_get(cs, hash, &l);
	if (d == NULL || !engine_chunk_refs(d, l, no_ref_fn, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the history store holds an unheld chunk not of the engine's formats"),
				 errdetail("A test before this one left it.")));
	pfree(d);
}

/*
 * test_vc_clear() returns int8: every reference of version control
 * removed, and what nothing holds then freed, by the engine's formats (its
 * branches' and commits' chunks, and all they alone held): the store as a
 * test found it, for the tests after it.  Every chunk it frees must be the
 * engine's (another test's, unheld, freed by these formats, would have the
 * wrong references let go of), and none is left unheld.  How many it freed.
 */
Datum
test_vc_clear(PG_FUNCTION_ARGS)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	const ChunkFormatRoutine *format = ChunkFormat;
	uint64		freed = 0;

	ChunkFormat = &EngineChunkFormat;
	PG_TRY();
	{
		VcRefs	   *refs = VcRefsOpen("pointers", cs);
		NameList	l = {NIL};
		ListCell   *lc;

		uint64		chunks,
					unheld;
		char	   *why = NULL;

		vc_refs_list(refs, "vc/", collect_fn, &l);
		foreach(lc, l.names)
			(void) vc_refs_remove(refs, lfirst(lc));
		cs_visit(cs, engine_only_fn, cs);
		freed = cs_free_unheld(cs);
		if (!cs_check(cs, &chunks, &unheld, &why) || unheld != 0)
			elog(ERROR, "the history store, cleared, has %llu unheld chunks: %s",
				 (unsigned long long) unheld, why ? why : "");
	}
	PG_FINALLY();
	{
		ChunkFormat = format;
	}
	PG_END_TRY();
	PG_RETURN_INT64((int64) freed);
}

/* test_vc_rows(branch text) returns int8: the rows of table t at its head */
Datum
test_vc_rows(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	VcRepo		r;
	uint64		n = 0;

	VC_CALL(r, 0,
	{
		VcBranch	b;
		VcTable		t;

		checkout(&r, branch);
		vc_head(&r, &b);
		if (vc_table_get(&r.ctx, &b.head_root, "t", &t))
			vc_must_ok(prolly_count(&r.ctx, &t.data, &n), &r.ctx);
		vc_branch_free(&b);
	});
	PG_RETURN_INT64((int64) n);
}

typedef struct RefsCount
{
	uint64		n;
	bool		fail;
} RefsCount;

static void
refs_count_fn(void *arg, const char *name, const PHash *hash)
{
	RefsCount  *c = arg;

	if (c->fail)
		elog(ERROR, "failed at reference \"%s\"", name);
	c->n++;
}

/*
 * test_vc_refs_count(prefix text, fail bool) returns int8: the references
 * under prefix, listed; with fail, an error at the first of them, as the
 * listing runs
 */
Datum
test_vc_refs_count(PG_FUNCTION_ARGS)
{
	char	   *prefix = text_to_cstring(PG_GETARG_TEXT_PP(0));
	RefsCount	c = {0, PG_GETARG_BOOL(1)};
	ChunkStore *cs = ChunkStoreOpen("pages");

	vc_refs_list(VcRefsOpen("pointers", cs), prefix, refs_count_fn, &c);
	PG_RETURN_INT64((int64) c.n);
}

/*
 * test_vc_sever(depth int4, discard bool) returns int8: every commit outside
 * the window of that depth moved into a new segment (discarded, with
 * discard); how many (0: none)
 */
Datum
test_vc_sever(PG_FUNCTION_ARGS)
{
	int32		depth = PG_GETARG_INT32(0);
	bool		discard = PG_GETARG_BOOL(1);
	VcRepo		r;
	uint64		n = 0;

	VC_CALL(r, 0,
	{
		VcWindow	w = {.depth = (uint32) depth};
		uint32		seg;
		VcSegment	s;

		if (vc_sever(&r, &w, &seg) != VC_OK)
			elog(ERROR, "no window of depth %d", depth);
		if (seg != VC_NO_SEGMENT && vc_segment_get(&r, seg, &s))
		{
			n = s.n;
			vc_segment_free(&s);
			if (discard && vc_segment_discard(&r, seg, NULL) != VC_OK)
				elog(ERROR, "segment %u not discarded", seg);
		}
	});
	PG_RETURN_INT64((int64) n);
}

/*
 * test_vc_branch_at(name text, branch text, msg text) returns bool: a
 * branch made at the commit on branch's line with message msg (found as
 * the call begins); false if it could not be (outside the window)
 */
Datum
test_vc_branch_at(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *msg = text_to_cstring(PG_GETARG_TEXT_PP(2));
	VcRepo		r;
	bool		made = false;

	VC_CALL(r, 0,
	{
		VcBranch	b;
		FindArg		f = {msg, false};

		checkout(&r, branch);
		vc_head(&r, &b);
		if (vc_log(&r, &b.head, 1000, 0, find_fn, &f) != VC_OK || !f.found)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" has no commit \"%s\"", branch, msg)));
		vc_branch_free(&b);
		made = vc_branch_create(&r, name, &f.hash) == VC_OK;
	});
	PG_RETURN_BOOL(made);
}

/*
 * test_vc_merge(branch text, other text) returns text: other merged into
 * branch, how ("up to date", "fast forward", "committed", "conflicts"),
 * or the error's code
 */
Datum
test_vc_merge(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *other = text_to_cstring(PG_GETARG_TEXT_PP(1));
	VcRepo		r;
	const char *how = NULL;
	int			rc = VC_OK;

	VC_CALL(r, 0,
	{
		VcMergeResult m;

		checkout(&r, branch);
		rc = vc_merge(&r, other, &m);
		if (rc == VC_OK)
		{
			how = m.status == VC_MERGE_UP_TO_DATE ? "up to date" :
				m.status == VC_MERGE_FAST_FORWARD ? "fast forward" :
				m.status == VC_MERGE_COMMITTED ? "committed" : "conflicts";
			vc_merge_result_free(&m);
		}
	});
	if (how == NULL)
		PG_RETURN_TEXT_P(cstring_to_text(psprintf("error %d", rc)));
	PG_RETURN_TEXT_P(cstring_to_text(how));
}

/*
 * test_vc_rewrite(rewrite bool) returns bool: the repository's history mode set,
 * REWRITE if rewrite, else KEEP
 */
Datum
test_vc_rewrite(PG_FUNCTION_ARGS)
{
	bool		on = PG_GETARG_BOOL(0);
	VcRepo		r;

	VC_CALL(r, 0,
	{
		vc_set_history_mode(&r, on ? VC_HISTORY_REWRITE : VC_HISTORY_KEEP);
	});
	PG_RETURN_BOOL(true);
}

static void
count_change_fn(void *arg, const VcTableChange *c)
{
	(*(uint64 *) arg)++;
}

/*
 * The history store's reads and writes, counted: the statement cancelled
 * as the cancel_after'th chunk is read, so the next check for interrupts,
 * deep in the operation reading it, ends it.
 */
static const ChunkStoreRoutine *counted_inner;
static ChunkStoreRoutine counted_routine;
static int64 cancel_after;
static int64 reads;
static int64 writes;
static int64 writes_at_cancel = -1;

static void
count_read(int n)
{
	reads += n;
	if (cancel_after > 0 && reads >= cancel_after)
	{
		cancel_after = 0;
		writes_at_cancel = writes;
		QueryCancelPending = true;
		InterruptPending = true;
	}
}

static void *
counted_get(ChunkStore *cs, const uint8 *hash, uint32 *len)
{
	count_read(1);
	return counted_inner->get(cs, hash, len);
}

static void
counted_get_many(ChunkStore *cs, const uint8 *hashes, int n, void **out, uint32 *lens)
{
	count_read(n);
	counted_inner->get_many(cs, hashes, n, out, lens);
}

static bool
counted_put(ChunkStore *cs, const void *data, uint32 len, uint8 *hash)
{
	writes++;
	return counted_inner->put(cs, data, len, hash);
}

static uint64
counted_put_many(ChunkStore *cs, int n, const uint8 *hashes, const void *const *datas,
				 const uint32 *lens)
{
	writes += n;
	return counted_inner->put_many(cs, n, hashes, datas, lens);
}

/*
 * test_vc_cancel(op text, after int8) returns int8: op ('diff' of main
 * and b, 'merge' of b into main, 'sever' to a window of 1) run, its
 * statement cancelled as it reads its after'th chunk (0: never), an error
 * at the next check for interrupts; how many chunks it read, if it ran to
 * its end first
 */
Datum
test_vc_cancel(PG_FUNCTION_ARGS)
{
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		after = PG_GETARG_INT64(1);
	VcRepo		r;

	reads = writes = 0;
	writes_at_cancel = -1;
	VC_CALL(r, 0,
	{
		VcBranch	main,
					b;
		ChunkStore *cs = r.ctx.cs;

		checkout(&r, "main");
		if (!vc_branch_get(&r, "main", &main) || !vc_branch_get(&r, "b", &b))
			elog(ERROR, "branches main and b are needed");
		counted_inner = cs->routine;
		counted_routine = *cs->routine;
		counted_routine.get = counted_get;
		counted_routine.put = counted_put;
		if (counted_inner->get_many)
			counted_routine.get_many = counted_get_many;
		if (counted_inner->put_many)
			counted_routine.put_many = counted_put_many;
		cancel_after = after;
		cs->routine = &counted_routine;
		PG_TRY(2);
		{
			if (strcmp(op, "diff") == 0)
			{
				uint64		n = 0;

				(void) vc_diff_commits(&r, &main.head, &b.head, 0, count_change_fn, &n);
			}
			else if (strcmp(op, "merge") == 0)
			{
				VcMergeResult m;

				if (vc_merge(&r, "b", &m) == VC_OK)
					vc_merge_result_free(&m);
			}
			else if (strcmp(op, "sever") == 0)
			{
				VcWindow	w = {.depth = 1};
				uint32		seg;

				(void) vc_sever(&r, &w, &seg);
			}
			else
				elog(ERROR, "no op \"%s\"", op);
		}
		PG_FINALLY(2);
		{
			cs->routine = counted_inner;
			cancel_after = 0;
		}
		PG_END_TRY(2);
		/* (ran to its end first: the cancel, if armed, taken back) */
		QueryCancelPending = false;
		InterruptPending = false;
		vc_branch_free(&main);
		vc_branch_free(&b);
	});
	PG_RETURN_INT64(reads);
}

/*
 * test_vc_cancel_writes() returns int8: how many chunks the last
 * test_vc_cancel had written when its cancel was armed (-1: never armed)
 */
Datum
test_vc_cancel_writes(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(writes_at_cancel);
}

/*
 * test_vc_transfer(op text) returns int4: op ('push', 'fetch' or 'deepen')
 * between the repository and itself, its code (a transfer's isolation is
 * checked before anything is done)
 */
Datum
test_vc_transfer(PG_FUNCTION_ARGS)
{
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(0));
	VcRepo		r;
	int			rc = VC_OK;

	VC_CALL(r, 0,
	{
		PHash		h;
		uint32		seg;
		VcSource	self = {"self", r.ctx.cs};
		VcBranch	b;

		if (strcmp(op, "push") == 0)
			rc = vc_push(&r, &r, "main");
		else if (strcmp(op, "fetch") == 0)
			rc = vc_fetch(&r, &r, "main", &h);
		else if (strcmp(op, "deepen") == 0)
		{
			vc_head(&r, &b);
			rc = vc_deepen(&r, &self, &b.head, 0, &seg);
			vc_branch_free(&b);
		}
		else
			elog(ERROR, "no op \"%s\"", op);
	});
	PG_RETURN_INT32(rc);
}

/* ---------------------------------------------------------- measurements */

/* (microseconds since start) */
static double
us_since(TimestampTz start)
{
	return (double) (GetCurrentTimestamp() - start);
}

/*
 * n rows of table t (made if the working set has none), first, first +
 * stride, ..., set to version, in one edit of the branch's working set
 */
static void edit_rows_every(VcRepo *r, uint64 first, uint64 n, uint64 stride,
							uint64 version);

static void
edit_rows(VcRepo *r, uint64 lo, uint64 hi, uint64 version)
{
	edit_rows_every(r, lo, hi - lo, 1, version);
}

/* Rows ks (n, ascending) set to version, in one edit of the working set. */
static void
edit_keys(VcRepo *r, const uint64 *ks, uint64 n, uint64 version)
{
	PHash		rv = vc_working_lock(r),
				out;
	VcTable		t;
	ProllyEdit *e = palloc_array(ProllyEdit, Max(n, 1));
	uint8	   *keys = palloc(Max(n, 1) * 8);
	uint8	   *vals = palloc(Max(n, 1) * 16);

	if (!vc_table_get(&r->ctx, &rv, "t", &t) &&
		vc_table_create(&r->ctx, &rv, "t", ddl, strlen(ddl), &rv) != VC_OK)
		elog(ERROR, "could not make table t");
	for (size_t i = 0; i < n; i++)
	{
		put_u64(keys + i * 8, ks[i]);
		put_u64(vals + i * 16, ks[i]);
		put_u64(vals + i * 16 + 8, version);
		e[i].key = keys + i * 8;
		e[i].klen = 8;
		e[i].val = vals + i * 16;
		e[i].vlen = 16;
		e[i].del = false;
	}
	if (n > 0 && vc_table_edit(&r->ctx, &rv, "t", e, n, &out) != VC_OK)
		elog(ERROR, "could not write table t");
	if (n > 0)
		vc_set_working(r, &out);
	pfree(e);
	pfree(keys);
	pfree(vals);
}

static void
edit_rows_every(VcRepo *r, uint64 first, uint64 n, uint64 stride, uint64 version)
{
	uint64	   *ks = palloc_array(uint64, Max(n, 1));

	for (uint64 i = 0; i < n; i++)
		ks[i] = first + i * stride;
	edit_keys(r, ks, n, version);
	pfree(ks);
}

/*
 * test_vc_round_trip(branch text, msg text) returns bytea: the repository
 * cloned into a memory store of this call's own, there a commit (message
 * msg || ' pushed') pushed back to the branch, another (' fetched') fetched
 * from it, and one more there (' theirs') pulled into the branch after one
 * of its own (' ours'), a merge; the fetched commit's hash.  The history
 * store has what each sent it as its transaction commits.
 */
Datum
test_vc_round_trip(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *msg = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bytea	   *out = palloc(VARHDRSZ + PHASH_LEN);
	VcRepo		r;

	SET_VARSIZE(out, VARHDRSZ + PHASH_LEN);
	VC_CALL(r, 0,
	{
		ChunkStore *mcs = MemoryChunkStoreCreate();
		VcRepo		laptop;
		VcBranch	b;
		VcMergeResult mr;
		PHash		sent,
					got;
		int			rc;

		PG_TRY(2);
		{
			rc = vc_repo_clone(&laptop, mcs, VcRefsOpen("memory", mcs), &prolly_default_policy,
							   &r, 0);
			if (rc != VC_OK)
				elog(ERROR, "the repository not cloned: %d", rc);
			vc_repo_set_name(&laptop, "laptop");

			sent = commit_msg(&laptop, branch, psprintf("%s pushed", msg));
			if ((rc = vc_push(&laptop, &r, branch)) != VC_OK)
				elog(ERROR, "the push refused: %d", rc);
			if (!vc_branch_get(&r, branch, &b) || !phash_eq(&b.head, &sent))
				elog(ERROR, "the push did not move \"%s\"", branch);
			vc_branch_free(&b);

			sent = commit_msg(&laptop, branch, psprintf("%s fetched", msg));
			if ((rc = vc_fetch(&r, &laptop, branch, &got)) != VC_OK)
				elog(ERROR, "the fetch refused: %d", rc);
			if (!phash_eq(&got, &sent))
				elog(ERROR, "the fetch brought another head");
			memcpy(VARDATA(out), got.b, PHASH_LEN);

			(void) commit_msg(&r, branch, psprintf("%s ours", msg));
			(void) commit_msg(&laptop, branch, psprintf("%s theirs", msg));
			if ((rc = vc_pull(&r, &laptop, branch, 0, &mr)) != VC_OK)
				elog(ERROR, "the pull refused: %d", rc);
			if (mr.status != VC_MERGE_COMMITTED)
				elog(ERROR, "the pull did not merge");
			vc_merge_result_free(&mr);
			vc_repo_free(&laptop);
		}
		PG_FINALLY(2);
		{
			/* (the store's references go with it: another call's may take its place) */
			MemoryVcRefsForget(mcs);
			MemoryChunkStoreDestroy(mcs);
		}
		PG_END_TRY(2);
	});
	PG_RETURN_BYTEA_P(out);
}

/*
 * test_vc_bench_commits(branch text, n int4, keys int4) returns float8: n
 * commits on branch, each setting one of keys rows (in turn, so the table
 * stays keys rows long however long history grows); microseconds a commit
 */
Datum
test_vc_bench_commits(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		n = PG_GETARG_INT32(1);
	int32		keys = PG_GETARG_INT32(2);
	static uint64 next = 0;
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		TimestampTz start;

		checkout(&r, branch);
		start = GetCurrentTimestamp();
		for (int32 i = 0; i < n; i++)
		{
			uint64		k = next++ % (uint64) Max(keys, 1);

			CHECK_FOR_INTERRUPTS();
			edit_rows(&r, k, k + 1, next);
			(void) vc_commit(&r, "bench");
		}
		us = us_since(start) / Max(n, 1);
	});
	PG_RETURN_FLOAT8(us);
}

/*
 * test_vc_bench_branches(prefix text, first int4, n int4) returns float8: n
 * branches made at the head checked out (main), prefix || first.. ;
 * microseconds a branch
 */
Datum
test_vc_bench_branches(PG_FUNCTION_ARGS)
{
	char	   *prefix = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		first = PG_GETARG_INT32(1);
	int32		n = PG_GETARG_INT32(2);
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		TimestampTz start = GetCurrentTimestamp();

		for (int32 i = 0; i < n; i++)
		{
			char	   *name = psprintf("%s%d", prefix, first + i);

			CHECK_FOR_INTERRUPTS();
			if (vc_branch_create(&r, name, NULL) != VC_OK)
				elog(ERROR, "could not make branch \"%s\"", name);
			pfree(name);
		}
		us = us_since(start) / Max(n, 1);
	});
	PG_RETURN_FLOAT8(us);
}

static void
count_log_fn(void *arg, const VcCommit *c)
{
	(*(uint64 *) arg)++;
}

/*
 * test_vc_bench_log(branch text, max int4) returns float8: the branch's log,
 * max commits of its first-parent line; microseconds for it all
 */
Datum
test_vc_bench_log(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		max = PG_GETARG_INT32(1);
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		VcBranch	b;
		uint64		n = 0;
		TimestampTz start;

		if (!vc_branch_get(&r, branch, &b))
			elog(ERROR, "there is no branch \"%s\"", branch);
		start = GetCurrentTimestamp();
		(void) vc_log(&r, &b.head, (size_t) max, 0, count_log_fn, &n);
		us = us_since(start);
		vc_branch_free(&b);
	});
	PG_RETURN_FLOAT8(us);
}

typedef struct DiffCount
{
	ProllyCtx  *ctx;
	uint64		rows;
} DiffCount;

static void
count_row_fn(void *arg, const ProllyDiff *d)
{
	((DiffCount *) arg)->rows++;
}

/* (a table changed: its rows that differ walked, counted) */
static void
count_change_bench_fn(void *arg, const VcTableChange *c)
{
	DiffCount  *dc = arg;

	if (c->has_from && c->has_to)
		vc_must_ok(prolly_diff(dc->ctx, &c->from.data, &c->to.data, count_row_fn, dc), dc->ctx);
}

/*
 * test_vc_bench_diff(a text, b text) returns (us float8, rows int8): the
 * diff of the two branches' heads, row by row; microseconds for it, and
 * the rows that differ
 */
Datum
test_vc_bench_diff(PG_FUNCTION_ARGS)
{
	char	   *a = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *b = text_to_cstring(PG_GETARG_TEXT_PP(1));
	double		us = 0;
	uint64		rows = 0;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false};
	VcRepo		r;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	VC_CALL(r, 0,
	{
		VcBranch	ba,
					bb;
		DiffCount	dc = {&r.ctx, 0};
		TimestampTz start;

		if (!vc_branch_get(&r, a, &ba) || !vc_branch_get(&r, b, &bb))
			elog(ERROR, "branches \"%s\" and \"%s\" are needed", a, b);
		start = GetCurrentTimestamp();
		if (vc_diff_commits(&r, &ba.head, &bb.head, 0, count_change_bench_fn, &dc) != VC_OK)
			elog(ERROR, "could not diff \"%s\" and \"%s\"", a, b);
		us = us_since(start);
		rows = dc.rows;
		vc_branch_free(&ba);
		vc_branch_free(&bb);
	});
	values[0] = Float8GetDatum(us);
	values[1] = Int64GetDatum((int64) rows);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * test_vc_bench_load(branch text, rows int8, batch int4) returns float8:
 * rows 0..rows-1 of table t set on branch, batch to an edit, then
 * committed; microseconds for it all
 */
Datum
test_vc_bench_load(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		rows = PG_GETARG_INT64(1);
	int32		batch = PG_GETARG_INT32(2);
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		TimestampTz start;

		checkout(&r, branch);
		start = GetCurrentTimestamp();
		for (int64 lo = 0; lo < rows; lo += Max(batch, 1))
		{
			CHECK_FOR_INTERRUPTS();
			edit_rows(&r, (uint64) lo, (uint64) Min(rows, lo + Max(batch, 1)), 0);
		}
		(void) vc_commit(&r, "load");
		us = us_since(start);
	});
	PG_RETURN_FLOAT8(us);
}

/*
 * test_vc_bench_edit(branch text, first int8, n int8, stride int8,
 * version int8) returns float8: n rows, first, first + stride, ..., set to
 * version on branch and committed; microseconds for it
 */
Datum
test_vc_bench_edit(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		first = PG_GETARG_INT64(1);
	int64		n = PG_GETARG_INT64(2);
	int64		stride = PG_GETARG_INT64(3);
	int64		version = PG_GETARG_INT64(4);
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		TimestampTz start;

		checkout(&r, branch);
		start = GetCurrentTimestamp();
		edit_rows_every(&r, (uint64) first, (uint64) n, (uint64) Max(stride, 1),
						(uint64) version);
		(void) vc_commit(&r, "edit");
		us = us_since(start);
	});
	PG_RETURN_FLOAT8(us);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/*
 * test_vc_bench_deep_commits(branch text, n int4, changes int4, rows int8)
 * returns float8: n commits on branch, each setting changes rows of the
 * first rows (distinct, at random); microseconds a commit
 */
Datum
test_vc_bench_deep_commits(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		n = PG_GETARG_INT32(1);
	int32		changes = Max(PG_GETARG_INT32(2), 1);
	int64		rows = Max(PG_GETARG_INT64(3), changes);
	static uint64 version = 0;
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		uint64	   *ks = palloc_array(uint64, changes);
		TimestampTz start;

		checkout(&r, branch);
		start = GetCurrentTimestamp();
		for (int32 i = 0; i < n; i++)
		{
			int32		m = 0;

			CHECK_FOR_INTERRUPTS();
			while (m < changes)
			{
				uint64		k = pg_prng_uint64_range(&pg_global_prng_state, 0, (uint64) rows - 1);
				bool		dup = false;

				for (int32 j = 0; j < m; j++)
					dup |= ks[j] == k;
				if (!dup)
					ks[m++] = k;
			}
			qsort(ks, changes, sizeof(uint64), cmp_u64);
			edit_keys(&r, ks, changes, ++version);
			(void) vc_commit(&r, "tx");
		}
		us = us_since(start) / Max(n, 1);
		pfree(ks);
	});
	PG_RETURN_FLOAT8(us);
}

/* (a commit's hashes collected, first parents, newest first) */
typedef struct LineAcc
{
	PHash	   *v;
	size_t		n,
				cap;
} LineAcc;

static void
line_fn(void *arg, const VcCommit *c)
{
	LineAcc    *a = arg;

	if (a->n == a->cap)
	{
		a->cap = Max(a->cap * 2, 1024);
		a->v = a->v ? repalloc_huge(a->v, a->cap * sizeof(PHash)) :
			palloc_extended(a->cap * sizeof(PHash), MCXT_ALLOC_HUGE);
	}
	a->v[a->n++] = c->hash;
}

/* A row diff of the table between two commits: milliseconds, and its rows. */
static double
deep_diff(VcRepo *r, const PHash *a, const PHash *b, uint64 *rows)
{
	VcCommit	ca,
				cb;
	VcTable		ta,
				tb;
	DiffCount	dc = {&r->ctx, 0};
	TimestampTz start = GetCurrentTimestamp();

	if (!vc_commit_load(&r->ctx, a, &ca) || !vc_commit_load(&r->ctx, b, &cb) ||
		!vc_table_get(&r->ctx, &ca.root, "t", &ta) || !vc_table_get(&r->ctx, &cb.root, "t", &tb))
		elog(ERROR, "no table to diff");
	vc_must_ok(prolly_diff(&r->ctx, &ta.data, &tb.data, count_row_fn, &dc), &r->ctx);
	vc_commit_release(&ca);
	vc_commit_release(&cb);
	*rows = dc.rows;
	return us_since(start) / 1000;
}

/*
 * test_vc_bench_deep_probe(branch text, old text) returns (held int8,
 * log100_us float8, mb_short_us float8, mb_long_ms float8, diff1_ms float8,
 * diff1_rows int8, diff1k_ms float8, diff1k_rows int8, diffhalf_ms float8,
 * diffhalf_rows int8): the branch's line (its commits, walked first and
 * not timed); a log of 100; the merge base with a branch made one commit
 * off ten commits back (and dropped again), and with old; and row diffs
 * to the commits 1, 1,000 and half the line back
 */
Datum
test_vc_bench_deep_probe(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *old = text_to_cstring(PG_GETARG_TEXT_PP(1));
	TupleDesc	tupdesc;
	Datum		values[10];
	bool		nulls[10] = {false};
	VcRepo		r;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	VC_CALL(r, 0,
	{
		VcBranch	b;
		LineAcc		line = {0};
		PHash		head,
					base,
					recent;
		uint64		n = 0,
					rows;
		TimestampTz start;
		size_t		back;

		if (!vc_branch_get(&r, branch, &b))
			elog(ERROR, "there is no branch \"%s\"", branch);
		head = b.head;
		vc_branch_free(&b);
		(void) vc_log(&r, &head, SIZE_MAX, 0, line_fn, &line);
		values[0] = Int64GetDatum((int64) line.n);

		start = GetCurrentTimestamp();
		(void) vc_log(&r, &head, 100, 0, count_log_fn, &n);
		values[1] = Float8GetDatum(us_since(start));

		back = Min(line.n - 1, 10);
		if (vc_branch_create(&r, "deep-recent", &line.v[back]) != VC_OK)
			elog(ERROR, "no branch made ten commits back");
		recent = commit_msg(&r, "deep-recent", "recent");
		start = GetCurrentTimestamp();
		if (vc_merge_base_in(&r, &head, &recent, 0, &base) != 1 ||
			!phash_eq(&base, &line.v[back]))
			elog(ERROR, "the merge base ten commits back not found");
		values[2] = Float8GetDatum(us_since(start));
		checkout(&r, branch);
		(void) vc_branch_delete(&r, "deep-recent");

		if (!vc_branch_get(&r, old, &b))
			elog(ERROR, "there is no branch \"%s\"", old);
		start = GetCurrentTimestamp();
		if (vc_merge_base_in(&r, &head, &b.head, 0, &base) != 1)
			elog(ERROR, "no merge base with \"%s\"", old);
		values[3] = Float8GetDatum(us_since(start) / 1000);
		vc_branch_free(&b);

		values[4] = Float8GetDatum(deep_diff(&r, &line.v[Min(line.n - 1, 1)], &head, &rows));
		values[5] = Int64GetDatum((int64) rows);
		values[6] = Float8GetDatum(deep_diff(&r, &line.v[Min(line.n - 1, 1000)], &head, &rows));
		values[7] = Int64GetDatum((int64) rows);
		values[8] = Float8GetDatum(deep_diff(&r, &line.v[line.n / 2], &head, &rows));
		values[9] = Int64GetDatum((int64) rows);
		pfree(line.v);
	});
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * test_vc_bench_sever(depth int4) returns float8: a sever with a window of
 * that depth, timed; microseconds
 */
Datum
test_vc_bench_sever(PG_FUNCTION_ARGS)
{
	int32		depth = PG_GETARG_INT32(0);
	double		us = 0;
	VcRepo		r;

	VC_CALL(r, 0,
	{
		VcWindow	w = {.depth = (uint32) depth};
		uint32		seg;
		TimestampTz start = GetCurrentTimestamp();

		if (vc_sever(&r, &w, &seg) != VC_OK)
			elog(ERROR, "no window of depth %d", depth);
		us = us_since(start);
	});
	PG_RETURN_FLOAT8(us);
}

/*
 * test_backend_private_mb() returns float8: this backend's private resident
 * memory (anonymous: not the shared buffers it has touched)
 */
Datum
test_backend_private_mb(PG_FUNCTION_ARGS)
{
	FILE	   *f = fopen("/proc/self/status", "r");
	char		line[256];
	double		kb = 0;

	if (f == NULL)
		PG_RETURN_NULL();
	while (fgets(line, sizeof(line), f))
		if (strncmp(line, "RssAnon:", 8) == 0)
			kb = strtod(line + 8, NULL);
	fclose(f);
	PG_RETURN_FLOAT8(kb / 1024);
}

/*
 * test_vc_bench_merge(branch text, other text) returns (status text,
 * us float8, gets int8, puts int8): other merged into branch, timed, with
 * the chunks the history store was asked for and asked to put
 */
Datum
test_vc_bench_merge(PG_FUNCTION_ARGS)
{
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *other = text_to_cstring(PG_GETARG_TEXT_PP(1));
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false};
	const char *how = "error";
	double		us = 0;
	uint64		gets = 0,
				puts = 0;
	VcRepo		r;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	VC_CALL(r, 0,
	{
		VcMergeResult m;
		ChunkStoreStats before;
		TimestampTz start;
		int			rc;

		checkout(&r, branch);
		before = *cs_stats(r.ctx.cs);
		start = GetCurrentTimestamp();
		rc = vc_merge(&r, other, &m);
		us = us_since(start);
		gets = cs_stats(r.ctx.cs)->gets - before.gets;
		puts = cs_stats(r.ctx.cs)->puts - before.puts;
		if (rc == VC_OK)
		{
			how = m.status == VC_MERGE_UP_TO_DATE ? "up to date" :
				m.status == VC_MERGE_FAST_FORWARD ? "fast forward" :
				m.status == VC_MERGE_COMMITTED ? "committed" : "conflicts";
			vc_merge_result_free(&m);
		}
	});
	values[0] = CStringGetTextDatum(how);
	values[1] = Float8GetDatum(us);
	values[2] = Int64GetDatum((int64) gets);
	values[3] = Int64GetDatum((int64) puts);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
