/*-------------------------------------------------------------------------
 *
 * chunk_xlog.c
 *	  Replay of the history store's WAL records (prolly/chunk_xlog.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "prolly/chunk_xlog.h"
#include "storage/standby.h"

void
chunk_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_CHUNK_INDEX_CLEANUP_INFO:
			{
				xl_chunk_index_cleanup_info *xlrec =
					(xl_chunk_index_cleanup_info *) XLogRecGetData(record);

				if (InHotStandby)
					ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
														false, xlrec->locator);
				break;
			}
		default:
			elog(PANIC, "chunk_redo: unknown op code %u", info);
	}
}
