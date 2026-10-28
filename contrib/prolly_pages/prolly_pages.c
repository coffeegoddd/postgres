/*-------------------------------------------------------------------------
 *
 * prolly_pages.c
 *	  Write whole pages back into a relation, for restoring a working set
 *	  from a commit image.
 *
 * Pages are written through the buffer manager under an exclusive lock on
 * the relation and logged as full page images, so the result is crash safe
 * and replicated like any other change.  Reading pages back out is done
 * with pageinspect's get_raw_page.
 *
 * contrib/prolly_pages/prolly_pages.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xloginsert.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "common/relpath.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(prolly_put_pages);
PG_FUNCTION_INFO_V1(prolly_truncate);

static Relation
open_for_write(Oid relid)
{
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to write raw pages")));
	rel = relation_open(relid, AccessExclusiveLock);
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" has no storage",
						RelationGetRelationName(rel))));
	return rel;
}

/*
 * prolly_put_pages(rel regclass, fork text, blocks int4[], pages bytea)
 *
 * pages holds one BLCKSZ page per block, back to back.  Writes each page at
 * its block, extending the fork (creating it if needed) when the block lies
 * past its end.  Returns the number written.
 */
Datum
prolly_put_pages(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *forkname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ArrayType  *barr = PG_GETARG_ARRAYTYPE_P(2);
	bytea	   *pages = PG_GETARG_BYTEA_PP(3);
	ForkNumber	fork = forkname_to_number(forkname);
	Relation	rel = open_for_write(relid);
	Datum	   *blocks;
	bool	   *bnulls;
	int			nb;

	deconstruct_array_builtin(barr, INT4OID, &blocks, &bnulls, &nb);
	if (VARSIZE_ANY_EXHDR(pages) != (size_t) nb * BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%d blocks need %zu bytes of pages, got %zu", nb,
						(size_t) nb * BLCKSZ, VARSIZE_ANY_EXHDR(pages))));

	for (int i = 0; i < nb; i++)
	{
		BlockNumber blk;
		const char *page = VARDATA_ANY(pages) + (size_t) i * BLCKSZ;
		Buffer		buf;

		if (bnulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null block number")));
		blk = (BlockNumber) DatumGetInt32(blocks[i]);

		if (smgrexists(RelationGetSmgr(rel), fork) &&
			blk < RelationGetNumberOfBlocksInFork(rel, fork))
			buf = ReadBufferExtended(rel, fork, blk, RBM_ZERO_AND_LOCK, NULL);
		else
			buf = ExtendBufferedRelTo(BMR_REL(rel), fork, NULL,
									  EB_CREATE_FORK_IF_NEEDED, blk + 1,
									  RBM_ZERO_AND_LOCK);

		START_CRIT_SECTION();
		memcpy(BufferGetPage(buf), page, BLCKSZ);
		MarkBufferDirty(buf);
		/* map forks keep their data in what a standard page calls the hole */
		if (RelationNeedsWAL(rel))
			log_newpage_buffer(buf, fork == MAIN_FORKNUM);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
	}

	/* index access methods cache their metapages in the relcache */
	CacheInvalidateRelcache(rel);
	relation_close(rel, NoLock);
	PG_RETURN_INT32(nb);
}

/*
 * prolly_truncate(rel regclass, nblocks int4)
 *
 * Shrinks the relation's main fork to nblocks, and its maps to match.
 */
Datum
prolly_truncate(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nblocks = PG_GETARG_INT32(1);
	Relation	rel = open_for_write(relid);

	if (nblocks < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("negative block count")));
	if ((BlockNumber) nblocks < RelationGetNumberOfBlocks(rel))
		RelationTruncate(rel, (BlockNumber) nblocks);
	CacheInvalidateRelcache(rel);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}
