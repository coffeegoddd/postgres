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
			appendStringInfo(buf, "next_block: %u, generation: %u, cut_from: %u",
							 ((xl_chunk_log_next_block *) rec)->next_block,
							 ((xl_chunk_log_next_block *) rec)->generation,
							 ((xl_chunk_log_next_block *) rec)->cut_from);
			break;
		case XLOG_CHUNK_LOG_INIT_PAGE:
			appendStringInfoString(buf, ((xl_chunk_log_init_page *) rec)->all_dead ?
								   "all dead" : "empty");
			break;
		case XLOG_CHUNK_LOG_SEAL_PAGE:
			appendStringInfo(buf, "off: %u%s", ((xl_chunk_log_seal *) rec)->off,
							 ((xl_chunk_log_seal *) rec)->init ? ", init" : "");
			break;
		case XLOG_CHUNK_LOG_APPEND:
			appendStringInfo(buf, "off: %u, len: %u%s",
							 ((xl_chunk_log_append *) rec)->off,
							 ((xl_chunk_log_append *) rec)->len,
							 ((xl_chunk_log_append *) rec)->init ? ", init" : "");
			break;
		case XLOG_CHUNK_LOG_BIND_INDEX:
			appendStringInfo(buf, "index: %u", ((xl_chunk_log_bind_index *) rec)->index);
			break;
		case XLOG_CHUNK_LOG_MARK_DEAD:
			appendStringInfo(buf, "off: %u, len: %u%s",
							 ((xl_chunk_log_mark_dead *) rec)->off,
							 ((xl_chunk_log_mark_dead *) rec)->len,
							 ((xl_chunk_log_mark_dead *) rec)->init ? ", init" : "");
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
		case XLOG_CHUNK_LOG_BIND_INDEX:
			return "BIND_INDEX";
	}
	return NULL;
}

void
chunk_index_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_CHUNK_INDEX_VACUUM:
			appendStringInfo(buf, "snapshotConflictHorizon: %u, nentries: %u",
							 ((xl_chunk_index_vacuum *) rec)->snapshotConflictHorizon,
							 ((xl_chunk_index_vacuum *) rec)->nentries);
			break;
		case XLOG_CHUNK_INDEX_ADD_PAGE:
			appendStringInfo(buf, "bucket: %u", ((xl_chunk_index_add_page *) rec)->bucket);
			break;
		case XLOG_CHUNK_INDEX_SET_COUNTS:
			appendStringInfo(buf, "slot: %u, refs: %u, roots: %u",
							 ((xl_chunk_index_slot *) rec)->slot,
							 ((xl_chunk_index_slot *) rec)->refs,
							 ((xl_chunk_index_slot *) rec)->roots);
			break;
		case XLOG_CHUNK_INDEX_SET_XMAX:
			appendStringInfo(buf, "slot: %u, xmax: %u",
							 ((xl_chunk_index_slot *) rec)->slot,
							 ((xl_chunk_index_slot *) rec)->xmax);
			break;
		case XLOG_CHUNK_INDEX_REMOVE_SLOT:
			appendStringInfo(buf, "slot: %u", ((xl_chunk_index_slot *) rec)->slot);
			break;
		case XLOG_CHUNK_INDEX_KEEP:
			appendStringInfo(buf, "nkept: %u", ((xl_chunk_index_keep *) rec)->nkept);
			break;
		case XLOG_CHUNK_INDEX_FOLD:
			{
				xl_chunk_index_fold *xlrec = (xl_chunk_index_fold *) rec;

				appendStringInfo(buf, "chunk slot: %u, change slot: %u%s, refs: %u, roots: %u",
								 xlrec->chunk_slot, xlrec->change_slot,
								 xlrec->same_page ? " (same page)" : "",
								 xlrec->refs, xlrec->roots);
				break;
			}
		case XLOG_CHUNK_INDEX_INIT_PAGE:
			{
				xl_chunk_index_init_page *xlrec = (xl_chunk_index_init_page *) rec;

				appendStringInfo(buf, "kind: %u, bucket: %u, split_at: %u, next: %u, len: %u",
								 xlrec->kind, xlrec->bucket, xlrec->split_at,
								 xlrec->next, xlrec->len);
				break;
			}
		case XLOG_CHUNK_INDEX_SPLIT:
			{
				xl_chunk_index_split *xlrec = (xl_chunk_index_split *) rec;

				appendStringInfo(buf, "new bucket: %u, lowmask: 0x%x, highmask: 0x%x, first: %u, next_block: %u, dir block: %u%s",
								 xlrec->new_bucket, xlrec->lowmask, xlrec->highmask,
								 xlrec->first, xlrec->next_block, xlrec->dir_block,
								 xlrec->new_dir ? " (new)" : "");
				break;
			}
		case XLOG_CHUNK_INDEX_BIND_LOG:
			appendStringInfo(buf, "log: %u, stamp: " UINT64_FORMAT,
							 ((xl_chunk_index_bind_log *) rec)->log,
							 ((xl_chunk_index_bind_log *) rec)->stamp);
			break;
	}
}

const char *
chunk_index_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CHUNK_INDEX_VACUUM:
			return "VACUUM";
		case XLOG_CHUNK_INDEX_ADD_ENTRY:
			return "ADD_ENTRY";
		case XLOG_CHUNK_INDEX_ADD_PAGE:
			return "ADD_PAGE";
		case XLOG_CHUNK_INDEX_SET_COUNTS:
			return "SET_COUNTS";
		case XLOG_CHUNK_INDEX_SET_XMAX:
			return "SET_XMAX";
		case XLOG_CHUNK_INDEX_KEEP:
			return "KEEP";
		case XLOG_CHUNK_INDEX_REMOVE_SLOT:
			return "REMOVE_SLOT";
		case XLOG_CHUNK_INDEX_FOLD:
			return "FOLD";
		case XLOG_CHUNK_INDEX_INIT_PAGE:
			return "INIT_PAGE";
		case XLOG_CHUNK_INDEX_SPLIT:
			return "SPLIT";
		case XLOG_CHUNK_INDEX_BIND_LOG:
			return "BIND_LOG";
	}
	return NULL;
}
