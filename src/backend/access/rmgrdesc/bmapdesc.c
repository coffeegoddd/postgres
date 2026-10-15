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
	}
	return NULL;
}
