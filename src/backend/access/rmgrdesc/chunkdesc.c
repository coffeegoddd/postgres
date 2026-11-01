/*-------------------------------------------------------------------------
 *
 * chunkdesc.c
 *	  rmgr descriptor routines for the history store (prolly/chunk_xlog.c)
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/chunkdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/chunk_xlog.h"

void
chunk_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_CHUNK_INDEX_CLEANUP_INFO)
	{
		xl_chunk_index_cleanup_info *xlrec = (xl_chunk_index_cleanup_info *) rec;

		appendStringInfo(buf, "rel %u/%u/%u; snapshotConflictHorizon: %u",
						 xlrec->locator.spcOid, xlrec->locator.dbOid,
						 xlrec->locator.relNumber,
						 xlrec->snapshotConflictHorizon);
	}
}

const char *
chunk_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CHUNK_INDEX_CLEANUP_INFO:
			return "INDEX_CLEANUP_INFO";
	}
	return NULL;
}
