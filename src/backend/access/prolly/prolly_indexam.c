/*-------------------------------------------------------------------------
 *
 * prolly_indexam.c
 *	  Index access method adapter for prolly index trees.
 *
 * A prolly index is a second tree from the same root set as its table
 * (specs/02-postgres-integration.md, A2).  Its operator classes follow the
 * btree operator class contract exactly, since key encodings must agree
 * with btree ordering (B6); so validation reuses btvalidate().
 *
 * The scan and maintenance callbacks need translation that isn't designed
 * yet and raise ERRCODE_FEATURE_NOT_SUPPORTED naming the callback.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_indexam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/nbtree.h"
#include "commands/vacuum.h"
#include "utils/fmgrprotos.h"

pg_noreturn static void
not_implemented(const char *callback)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("index access method \"%s\" does not implement %s yet",
					"prolly", callback)));
	pg_unreachable();
}

static IndexBuildResult *
prolly_ambuild(Relation heapRelation, Relation indexRelation,
			   IndexInfo *indexInfo)
{
	not_implemented("ambuild");
}

static void
prolly_ambuildempty(Relation indexRelation)
{
	not_implemented("ambuildempty");
}

static bool
prolly_aminsert(Relation indexRelation, Datum *values, bool *isnull,
				ItemPointer heap_tid, Relation heapRelation,
				IndexUniqueCheck checkUnique, bool indexUnchanged,
				IndexInfo *indexInfo)
{
	not_implemented("aminsert");
}

static IndexBulkDeleteResult *
prolly_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
					IndexBulkDeleteCallback callback, void *callback_state)
{
	not_implemented("ambulkdelete");
}

static IndexBulkDeleteResult *
prolly_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	not_implemented("amvacuumcleanup");
}

static bool
prolly_amcanreturn(Relation indexRelation, int attno)
{
	not_implemented("amcanreturn");
}

static void
prolly_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					  Cost *indexStartupCost, Cost *indexTotalCost,
					  Selectivity *indexSelectivity, double *indexCorrelation,
					  double *indexPages)
{
	not_implemented("amcostestimate");
}

static bytea *
prolly_amoptions(Datum reloptions, bool validate)
{
	not_implemented("amoptions");
}

static IndexScanDesc
prolly_ambeginscan(Relation indexRelation, int nkeys, int norderbys)
{
	not_implemented("ambeginscan");
}

static void
prolly_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
				ScanKey orderbys, int norderbys)
{
	not_implemented("amrescan");
}

static bool
prolly_amgettuple(IndexScanDesc scan, ScanDirection direction)
{
	not_implemented("amgettuple");
}

static int64
prolly_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	not_implemented("amgetbitmap");
}

static void
prolly_amendscan(IndexScanDesc scan)
{
	not_implemented("amendscan");
}

static void
prolly_ammarkpos(IndexScanDesc scan)
{
	not_implemented("ammarkpos");
}

static void
prolly_amrestrpos(IndexScanDesc scan)
{
	not_implemented("amrestrpos");
}

Datum
prolly_indexam_handler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		/* operator classes follow the btree contract */
		.amstrategies = BTMaxStrategyNumber,
		.amsupport = BTNProcs,
		.amoptsprocnum = BTOPTIONS_PROC,
		.amcanorder = true,
		.amcanorderbyop = false,
		.amcanhash = false,
		.amconsistentequality = true,
		.amconsistentordering = true,
		.amcanbackward = true,
		.amcanunique = true,
		.amcanmulticol = true,
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = true,
		.amstorage = false,
		.amclusterable = true,
		.ampredlocks = true,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = true,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL,
		.amkeytype = InvalidOid,

		.ambuild = prolly_ambuild,
		.ambuildempty = prolly_ambuildempty,
		.aminsert = prolly_aminsert,
		.aminsertcleanup = NULL,
		.ambulkdelete = prolly_ambulkdelete,
		.amvacuumcleanup = prolly_amvacuumcleanup,
		.amcanreturn = prolly_amcanreturn,
		.amcostestimate = prolly_amcostestimate,
		.amgettreeheight = NULL,
		.amoptions = prolly_amoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = btvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = prolly_ambeginscan,
		.amrescan = prolly_amrescan,
		.amgettuple = prolly_amgettuple,
		.amgetbitmap = prolly_amgetbitmap,
		.amendscan = prolly_amendscan,
		.ammarkpos = prolly_ammarkpos,
		.amrestrpos = prolly_amrestrpos,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = NULL,
	};

	PG_RETURN_POINTER(&amroutine);
}
