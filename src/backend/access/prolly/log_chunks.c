/*-------------------------------------------------------------------------
 *
 * log_chunks.c
 *	  Log implementation "chunks": the WAL carries whole nodes (D1).
 *
 * D1's nodes are immutable and written once, so the WAL form is the node
 * itself: log_node() writes a Prolly record holding a full image of the
 * node's page.  The page is a standard page, so the image leaves out the
 * free space between pd_lower and pd_upper.  Redo restores the image.
 *
 * Row-level records (log_mutation) are not written: D1 has no logical WAL,
 * and logical decoding is deferred (open question 31).  The commit record
 * (log_commit) comes with the commit pipeline in roadmap step 4e.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/log_chunks.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "storage/bufmgr.h"

#define CHUNKS_IMPL_ID	1

/* Prolly record types (high bits of info) */
#define XLOG_PROLLY_NODE	0x10	/* a node's full page image */

static void
chunks_log_node(Relation tree, Buffer buf, bool is_new)
{
	xl_prolly_header hdr;
	XLogRecPtr	recptr;

	hdr.impl_id = CHUNKS_IMPL_ID;
	XLogBeginInsert();
	XLogRegisterData(&hdr, SizeOfProllyHeader);
	XLogRegisterBuffer(0, buf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
	recptr = XLogInsert(RM_PROLLY_ID, XLOG_PROLLY_NODE);
	PageSetLSN(BufferGetPage(buf), recptr);
}

static void
chunks_log_mutation(Relation tree, const ProllyKey *key, const ProllyValue *value)
{
	/* D1's WAL is its nodes; there is nothing to log per row. */
}

static void
chunks_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & PROLLY_XLOG_TYPE_MASK;
	Buffer		buf;

	switch (info)
	{
		case XLOG_PROLLY_NODE:
			if (XLogReadBufferForRedo(record, 0, &buf) != BLK_RESTORED)
				elog(PANIC, "prolly node record has no page image");
			UnlockReleaseBuffer(buf);
			break;
		default:
			elog(PANIC, "prolly log \"chunks\": unknown record type 0x%02X", info);
	}
}

const ProllyLogRoutine prolly_log_chunks = {
	.info = {
		.iface = PROLLY_IFACE_LOG,
		.impl_id = CHUNKS_IMPL_ID,
		.format_version = 1,
		.name = "chunks",
		.description = "whole nodes as full page images (D1)",
	},
	.log_node = chunks_log_node,
	.log_mutation = chunks_log_mutation,
	.redo = chunks_redo,
};
