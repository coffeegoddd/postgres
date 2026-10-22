/*-------------------------------------------------------------------------
 *
 * fold_verify.c
 *	  Incremental folds checked against full ones, off the commit path.
 *
 * A fold of a table from its baseline (its tree at the last fold, and the
 * rows changed since) must make the tree a fold of every row would.  The
 * verifier checks that it does: by one snapshot, it folds the session
 * branch's tables from their baselines, then each table sampled in full,
 * and compares the two trees.  One that differs is warned of (or, with
 * prolly.fold_verify_fatal, a PANIC, for testing), counted in
 * pg_stat_prolly (fold_verify_mismatches), and its table's change log told
 * every row may have changed: its next fold reads it in full, so the wrong
 * tree goes into no later commit.  Commits made before keep the tree they
 * got: this detects and limits, it doesn't prevent.
 *
 * Autovacuum runs it in each database at prolly.fold_verify_rate (the
 * fraction of tables checked at a pass; 0, the default, none), on the
 * default branch; dolt_verify_folds() runs it on the session's.
 *
 * A working set the refs took from an earlier fold instead of folding
 * (#482) is checked too, with debug_prolly_verify_fold_caches: folded
 * again, and the two roots compared (prolly_check_fold_cache()).  One that
 * differs is an error, so the earlier fold's root goes into nothing.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/fold_verify.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_canon.h"
#include "access/prolly_changelog.h"
#include "access/prolly_config.h"
#include "access/prolly_fold.h"
#include "access/prolly_refs.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "common/pg_prng.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/regproc.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

bool		prolly_fold_verifying = false;
double		prolly_fold_verify_rate = 0;
bool		prolly_fold_verify_fatal = false;
bool		debug_prolly_verify_fold_caches = false;

/* A folded root's tables: their names and data trees, in name order. */
typedef struct FoldedTable
{
	char	   *name;
	ProllyNodeRef data;
} FoldedTable;

static void
collect_table(const char *name, const ProllyNodeRef *data,
			  const ProllyValue *description, const char *unreadable,
			  void *arg)
{
	List	  **tables = arg;
	FoldedTable *t = palloc(sizeof(FoldedTable));

	t->name = pstrdup(name);
	t->data = *data;
	*tables = lappend(*tables, t);
}

static const FoldedTable *
find_table(List *tables, const char *name)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		FoldedTable *t = lfirst(lc);

		if (strcmp(t->name, name) == 0)
			return t;
	}
	return NULL;
}

static bool
same_tree(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	return a->hash.len == b->hash.len &&
		memcmp(a->hash.bytes, b->hash.bytes, a->hash.len) == 0;
}

static char *
tree_hex(const ProllyNodeRef *ref)
{
	StringInfoData s;

	initStringInfo(&s);
	for (int i = 0; i < ref->hash.len; i++)
		appendStringInfo(&s, "%02x", ref->hash.bytes[i]);
	return s.data;
}

int
prolly_verify_folds(double rate, ProllyFoldVerifyVisit visit, void *arg)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef incremental;
	List	   *folded = NIL;
	int			mismatches = 0;
	ListCell   *lc;

	prolly_database_components(&dc);
	if (dc.refs == NULL || dc.canonicalizer == NULL ||
		!PROLLY_HAS(dc.refs, read_working_set))
		return 0;

	/* (both folds by one snapshot: the same rows, whichever way read) */
	PushActiveSnapshot(GetTransactionSnapshot());
	if (!PROLLY_CALL(dc.refs, read_working_set, NULL, InvalidOid, &incremental,
					 NULL))
	{
		PopActiveSnapshot();
		return 0;
	}
	PROLLY_CALL(dc.canonicalizer, tables, &incremental, NULL, collect_table, &folded);
	foreach(lc, folded)
	{
		FoldedTable *t = lfirst(lc);
		ProllyNodeRef full;
		List	   *fulls = NIL;
		const FoldedTable *ft;
		Oid			relid;
		bool		matches;

		CHECK_FOR_INTERRUPTS();
		if (rate < 1 && pg_prng_double(&pg_global_prng_state) >= rate)
			continue;
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(t->name, NULL)),
								 AccessShareLock, true);
		if (!OidIsValid(relid))
			continue;

		prolly_fold_verifying = true;
		PG_TRY();
		{
			(void) PROLLY_CALL(dc.refs, read_working_set, NULL, relid, &full, NULL);
		}
		PG_FINALLY();
		{
			prolly_fold_verifying = false;
		}
		PG_END_TRY();
		PROLLY_CALL(dc.canonicalizer, tables, &full, NULL, collect_table, &fulls);
		ft = find_table(fulls, t->name);
		matches = ft != NULL && same_tree(&ft->data, &t->data);
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_VERIFIED, 1);
		if (!matches)
		{
			Relation	rel;

			mismatches++;
			pgstat_count_prolly(PGSTAT_PROLLY_FOLD_VERIFY_MISMATCHES, 1);
			ereport(prolly_fold_verify_fatal ? PANIC : WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly fold of table %s from its baseline differs from a fold of every row",
							t->name),
					 errdetail("From the baseline: %s; in full: %s.",
							   tree_hex(&t->data),
							   ft ? tree_hex(&ft->data) : "none"),
					 errhint("Its next fold reads it in full.")));

			/* (its log told every row may have changed: the next fold reads all) */
			{
				ProllyComponents c;

				rel = table_open(relid, RowExclusiveLock);
				prolly_components_for_relation(rel, &c);
				if (c.change_log)
					PROLLY_CALL(c.change_log, note, rel, NULL, NULL, NULL);
				table_close(rel, RowExclusiveLock);

				/* (nor is a working set taken from an earlier fold (#482)) */
				prolly_forget_folds();
			}
		}
		if (visit)
			visit(t->name, matches, arg);
	}
	PopActiveSnapshot();
	return mismatches;
}

void
prolly_check_fold_cache(const char *branch, const ProllyNodeRef *taken,
						const ProllyNodeRef *folded, const char *left_out)
{
	if (left_out != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly working set of branch \"%s\" taken from an earlier fold left nothing out, a fold now leaves out %s",
						branch, left_out)));
	if (!same_tree(taken, folded))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly working set of branch \"%s\" taken from an earlier fold differs from a fold now",
						branch),
				 errdetail("Taken: %s; folded: %s.", tree_hex(taken),
						   tree_hex(folded))));
}

/* dolt_verify_folds()'s rows. */
static void
put_verified(const char *name, bool matches, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2];
	bool		nulls[2] = {0};

	values[0] = CStringGetTextDatum(name);
	values[1] = BoolGetDatum(matches);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_verify_folds(sample float8 DEFAULT 1, OUT table_name text, OUT
 *					 matches bool) returns setof record
 *
 * The session branch's tables (a sample fraction of them), each folded
 * from its baseline and in full by one snapshot: whether the trees match.
 * One that doesn't is warned of, counted, and read in full at its next
 * fold.
 */
Datum
dolt_verify_folds(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	double		sample = PG_ARGISNULL(0) ? 1 : PG_GETARG_FLOAT8(0);

	if (sample < 0 || sample > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a sample is a fraction from 0 to 1")));
	InitMaterializedSRF(fcinfo, 0);
	(void) prolly_verify_folds(sample, put_verified, rsinfo);
	return (Datum) 0;
}
