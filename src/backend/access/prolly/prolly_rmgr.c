/*-------------------------------------------------------------------------
 *
 * prolly_rmgr.c
 *	  WAL resource-manager adapters for prolly storage (A3).
 *
 * Redo for the Prolly and ProllyStore resource managers is dispatched to the
 * implementation named in the record's xl_prolly_header: a log
 * implementation for Prolly, a node store implementation for ProllyStore.
 * desc and identify are generic and live in rmgrdesc/prollydesc.c.
 *
 * Masking for wal_consistency_checking is common to both: prolly pages are
 * standard pages, so the LSN, checksum and the unused space between pd_lower
 * and pd_upper are masked, as for other page types.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_rmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/prolly_config.h"
#include "access/prolly_xlog.h"

static const ProllyImplInfo *
record_impl(XLogReaderState *record, ProllyInterface iface)
{
	xl_prolly_header hdr;
	const ProllyImplInfo *impl;

	if (XLogRecGetDataLen(record) < SizeOfProllyHeader)
		elog(PANIC, "prolly WAL record has no header");
	memcpy(&hdr, XLogRecGetData(record), SizeOfProllyHeader);

	impl = prolly_lookup_impl_id(iface, hdr.impl_id);
	if (impl == NULL)
		elog(PANIC, "prolly WAL record was written by unknown %s implementation %u",
			 prolly_interface_name(iface), hdr.impl_id);
	return impl;
}

void
prolly_redo(XLogReaderState *record)
{
	const ProllyLogRoutine *log = (const ProllyLogRoutine *)
		record_impl(record, PROLLY_IFACE_LOG);

	PROLLY_CALL(log, redo, record);
}

void
prolly_store_redo(XLogReaderState *record)
{
	const ProllyNodeStoreRoutine *store = (const ProllyNodeStoreRoutine *)
		record_impl(record, PROLLY_IFACE_NODE_STORE);

	PROLLY_CALL(store, redo, record);
}

void
prolly_mask(char *pagedata, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(pagedata);
	mask_unused_space(pagedata);
}
