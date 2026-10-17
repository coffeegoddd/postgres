/*-------------------------------------------------------------------------
 *
 * bmapdesc.c
 *	  rmgr descriptor routines for the block-mapped storage manager
 *	  (storage/smgr/bmap.c)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/bmapdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bmap_xlog.h"

void
bmap_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BMAP_CREATE:
		case XLOG_BMAP_TRUNCATE:
		case XLOG_BMAP_FREE:
		case XLOG_BMAP_FOLD:
		case XLOG_BMAP_CHANGES:
			{
				xl_bmap_rel *xlrec = (xl_bmap_rel *) rec;

				appendStringInfo(buf, "rel %u/%u/%u",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber);
				break;
			}
		case XLOG_BMAP_EXTEND:
			{
				xl_bmap_extend *xlrec = (xl_bmap_extend *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; hw %u -> %u; reused %u",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber,
								 xlrec->old_hw, xlrec->new_hw, xlrec->nreused);
				break;
			}
		case XLOG_BMAP_SHARE:
		case XLOG_BMAP_SHARED:
		case XLOG_BMAP_SHARE_CHANGES:
			{
				xl_bmap_share *xlrec = (xl_bmap_share *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; copy %u",
								 xlrec->src.spcOid, xlrec->src.dbOid,
								 xlrec->src.relNumber, xlrec->dst.relNumber);
				break;
			}
		case XLOG_BMAP_REMAP:
			{
				xl_bmap_remap *xlrec = (xl_bmap_remap *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; blk %u; block %u -> %u%s%s%s; hw %u -> %u",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber, xlrec->blkno,
								 xlrec->old_block, xlrec->new_block,
								 (xlrec->flags & BMAP_REMAP_ZERO) ? " (zeroed)" : "",
								 (xlrec->flags & BMAP_REMAP_LISTED) ? " (listed)" : "",
								 (xlrec->flags & BMAP_REMAP_OWN) ? " (own)" : "",
								 xlrec->old_hw, xlrec->new_hw);
				break;
			}
		case XLOG_BMAP_UNREF:
			{
				xl_bmap_unref *xlrec = (xl_bmap_unref *) rec;

				appendStringInfo(buf, "rel %u/%u/%u%s",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber,
								 (xlrec->flags & BMAP_UNREF_MAP) ? "; map" : "");
				break;
			}
		default:
			break;
	}
}

const char *
bmap_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_BMAP_INIT:
			return "INIT";
		case XLOG_BMAP_CREATE:
			return "CREATE";
		case XLOG_BMAP_EXTEND:
			return "EXTEND";
		case XLOG_BMAP_TRUNCATE:
			return "TRUNCATE";
		case XLOG_BMAP_FREE:
			return "FREE";
		case XLOG_BMAP_SHARE:
			return "SHARE";
		case XLOG_BMAP_SHARED:
			return "SHARED";
		case XLOG_BMAP_UNREF:
			return "UNREF";
		case XLOG_BMAP_REMAP:
			return "REMAP";
		case XLOG_BMAP_FOLD:
			return "FOLD";
		case XLOG_BMAP_CHANGES:
			return "CHANGES";
		case XLOG_BMAP_SHARE_CHANGES:
			return "SHARE_CHANGES";
	}
	return NULL;
}
