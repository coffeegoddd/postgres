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
		case XLOG_BMAP_SHARED:
			{
				xl_bmap_share *xlrec = (xl_bmap_share *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; copy %u%s",
								 xlrec->src.spcOid, xlrec->src.dbOid,
								 xlrec->src.relNumber, xlrec->dst.relNumber,
								 (xlrec->flags & BMAP_PRESERVE_LOGGED) ?
								 " (images logged)" : "");
				if (xlrec->flags & BMAP_SHARE_SYNCED)
					appendStringInfo(buf, "; synced since %X/%08X",
									 LSN_FORMAT_ARGS(xlrec->synced_since));
				break;
			}
		case XLOG_BMAP_PRESERVE:
			{
				BlockNumber phys;

				memcpy(&phys, rec, sizeof(phys));
				appendStringInfo(buf, "block %u", phys);
				break;
			}
		case XLOG_BMAP_REMAP:
			{
				xl_bmap_remap *xlrec = (xl_bmap_remap *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; blk %u; ",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber, xlrec->blkno);
				/* (a hole, given its first block, had none) */
				if (xlrec->old_block == InvalidBlockNumber)
					appendStringInfoString(buf, "hole");
				else
					appendStringInfo(buf, "block %u", xlrec->old_block);
				appendStringInfo(buf, " -> %u%s%s; hw %u -> %u",
								 xlrec->new_block,
								 (xlrec->flags & BMAP_REMAP_ZERO) ? " (zeroed)" : "",
								 (xlrec->flags & BMAP_REMAP_COPIED) ? " (copied)" : "",
								 xlrec->old_hw, xlrec->new_hw);
				break;
			}
		case XLOG_BMAP_PASSED:
			{
				xl_bmap_passed *xlrec = (xl_bmap_passed *) rec;

				appendStringInfo(buf, "horizon %X/%08X; copied %X/%08X",
								 LSN_FORMAT_ARGS(xlrec->horizon),
								 LSN_FORMAT_ARGS(xlrec->copied));
				break;
			}
		case XLOG_BMAP_UNREF:
			{
				xl_bmap_unref *xlrec = (xl_bmap_unref *) rec;

				appendStringInfo(buf, "rel %u/%u/%u%s",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber,
								 (xlrec->flags & BMAP_UNREF_MAP) ? "; map" :
								 (xlrec->flags & BMAP_UNREF_ROOTS) ? "; roots' counts" : "");
				break;
			}
		case XLOG_BMAP_TREE:
			{
				xl_bmap_tree *xlrec = (xl_bmap_tree *) rec;

				appendStringInfo(buf, "rel %u/%u/%u; ",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber);
				if (xlrec->op == BMAP_TREE_NODE)
					appendStringInfo(buf, "node %u%s; hw %u -> %u",
									 xlrec->node,
									 (xlrec->flags & BMAP_TREE_INSTALL) ? " (installed)" : "",
									 xlrec->old_hw, xlrec->new_hw);
				else if (xlrec->op == BMAP_TREE_HOLD)
					appendStringInfo(buf, "hold children of %u", xlrec->from);
				else if (xlrec->op == BMAP_TREE_COPY)
					appendStringInfo(buf, "copy %u -> %u; hw %u -> %u",
									 xlrec->from, xlrec->node,
									 xlrec->old_hw, xlrec->new_hw);
				else if (xlrec->op == BMAP_TREE_SHARE)
					appendStringInfo(buf, "share of %u%s", xlrec->from,
									 (xlrec->flags & BMAP_TREE_OF_ROOT) ?
									 "'s root" : "");
				else if (xlrec->op == BMAP_TREE_CAPTURE)
					appendStringInfoString(buf, "root captured (images logged)");
				else if (xlrec->op == BMAP_TREE_ROOT_SLOT)
					appendStringInfoString(buf, "root slot");
				else if (xlrec->op == BMAP_TREE_EPOCH)
					appendStringInfoString(buf, "epoch raised");
				else if (xlrec->op == BMAP_TREE_STAMP)
					appendStringInfo(buf, "node %u stamped, born again", xlrec->node);
				else if (xlrec->op == BMAP_TREE_FREE_SPACE)
					appendStringInfoString(buf, "free space known");
				else if (xlrec->op == BMAP_TREE_CREATOR)
					appendStringInfoString(buf, "taken to be dropped, its transaction never committed");
				else if (xlrec->op == BMAP_TREE_UNHOLD)
					appendStringInfo(buf, "children of %u held once fewer, it claimed", xlrec->from);
				else
					appendStringInfo(buf, "op %u", xlrec->op);
				break;
			}
		case XLOG_BMAP_RELEASE:
			{
				xl_bmap_release *xlrec = (xl_bmap_release *) rec;

				appendStringInfo(buf, "store %u/%u; ",
								 xlrec->locator.spcOid, xlrec->locator.dbOid);
				if (xlrec->op == BMAP_RELEASE_QUEUE)
					appendStringInfoString(buf, "queue");
				else if (xlrec->op == BMAP_RELEASE_DRAIN)
					appendStringInfo(buf, "drain %u", xlrec->npopped);
				else
					appendStringInfo(buf, "op %u", xlrec->op);
				break;
			}
		case XLOG_BMAP_SHRINK:
			{
				xl_bmap_shrink *xlrec = (xl_bmap_shrink *) rec;

				appendStringInfo(buf, "store %u/%u; hw %u -> %u",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->old_hw, xlrec->new_hw);
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
		case XLOG_BMAP_PASSED:
			return "PASSED";
		case XLOG_BMAP_SHARED:
			return "SHARED";
		case XLOG_BMAP_UNREF:
			return "UNREF";
		case XLOG_BMAP_REMAP:
			return "REMAP";
		case XLOG_BMAP_SHRINK:
			return "SHRINK";
		case XLOG_BMAP_PRESERVE:
			return "PRESERVE";
		case XLOG_BMAP_TREE:
			return "TREE";
		case XLOG_BMAP_RELEASE:
			return "RELEASE";
	}
	return NULL;
}
