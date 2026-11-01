/*-------------------------------------------------------------------------
 *
 * chunkdesc.c
 *	  rmgr descriptor routines for the history store's chunk log and chunk
 *	  index (prolly/chunk_log.c, prolly/chunk_xlog.c)
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
chunk_log_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_CHUNK_LOG_NEXT_BLOCK:
			appendStringInfo(buf, "next_block: %u",
							 ((xl_chunk_log_next_block *) rec)->next_block);
			break;
		case XLOG_CHUNK_LOG_INIT_PAGE:
			appendStringInfoString(buf, ((xl_chunk_log_init_page *) rec)->all_dead ?
								   "all dead" : "empty");
			break;
		case XLOG_CHUNK_LOG_APPEND:
			appendStringInfo(buf, "off: %u, len: %u",
							 ((xl_chunk_log_append *) rec)->off,
							 ((xl_chunk_log_append *) rec)->len);
			break;
		case XLOG_CHUNK_LOG_MARK_DEAD:
			appendStringInfo(buf, "len: %u", ((xl_chunk_log_mark_dead *) rec)->len);
			break;
	}
}

const char *
chunk_log_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CHUNK_LOG_NEXT_BLOCK:
			return "NEXT_BLOCK";
		case XLOG_CHUNK_LOG_INIT_PAGE:
			return "INIT_PAGE";
		case XLOG_CHUNK_LOG_SEAL_PAGE:
			return "SEAL_PAGE";
		case XLOG_CHUNK_LOG_APPEND:
			return "APPEND";
		case XLOG_CHUNK_LOG_MARK_DEAD:
			return "MARK_DEAD";
	}
	return NULL;
}

void
chunk_index_desc(StringInfo buf, XLogReaderState *record)
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
chunk_index_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CHUNK_INDEX_CLEANUP_INFO:
			return "INDEX_CLEANUP_INFO";
	}
	return NULL;
}
