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

#include "common/hashfn.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "prolly/engine/chunkfmt.h"
#include "prolly/engine/vc.h"
#include "utils/builtins.h"

#include "test_prolly.h"

PG_FUNCTION_INFO_V1(test_vc_open);
PG_FUNCTION_INFO_V1(test_vc_commit);
PG_FUNCTION_INFO_V1(test_vc_branch);
PG_FUNCTION_INFO_V1(test_vc_branch_delete);
PG_FUNCTION_INFO_V1(test_vc_reset);
PG_FUNCTION_INFO_V1(test_vc_log);
PG_FUNCTION_INFO_V1(test_vc_working_has);
PG_FUNCTION_INFO_V1(test_vc_head_in_window);
PG_FUNCTION_INFO_V1(test_vc_clear);
PG_FUNCTION_INFO_V1(test_vc_rows);
PG_FUNCTION_INFO_V1(test_vc_sever);

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
		PHash		rv,
					out;
		VcTable		t;
		uint8		key[8],
					val[16];
		ProllyEdit	e = {key, 8, val, 16, false};

		checkout(&r, branch);
		/* (an edit: locked as it reads, so a second on the branch waits) */
		rv = vc_working_lock(&r);
		if (!vc_table_get(&r.ctx, &rv, "t", &t))
		{
			if (vc_table_create(&r.ctx, &rv, "t", ddl, strlen(ddl), &rv) != VC_OK)
				elog(ERROR, "could not make table t");
		}
		put_u64(key, msg_key(msg));
		put_u64(val, msg_key(msg));
		put_u64(val + 8, 1);
		if (vc_table_edit(&r.ctx, &rv, "t", &e, 1, &out) != VC_OK)
			elog(ERROR, "could not write table t");
		vc_set_working(&r, &out);
		(void) vc_commit(&r, msg);
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

/*
 * test_vc_sever(depth int4) returns int8: every commit outside the window
 * of that depth moved into a new segment; how many (0: none)
 */
Datum
test_vc_sever(PG_FUNCTION_ARGS)
{
	int32		depth = PG_GETARG_INT32(0);
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
		}
	});
	PG_RETURN_INT64((int64) n);
}
