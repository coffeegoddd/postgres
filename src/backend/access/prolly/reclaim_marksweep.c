/*-------------------------------------------------------------------------
 *
 * reclaim_marksweep.c
 *	  Reclamation implementation "mark_sweep": collect the node store's
 *	  garbage by marking from every live root, as dolt gc does.
 *
 * The head of each database's refs branch isn't a working set but the
 * refs' own tree, which the refs implementation walks (commits, the
 * working sets they recorded, their roots).
 *
 * collect() takes the store lock exclusively, which waits for every
 * transaction writing nodes (or keeping nodes it found in the store, or a
 * prepared one) to end and keeps new ones waiting until the current
 * transaction ends.  It then marks every node reachable from a root the
 * commit pipeline holds, in every database: branch heads, the roots older
 * snapshots still read, and roots enqueued for commit.  A walk skips what
 * lies below a node already marked, so trees that share most of their
 * nodes (successive commits) cost what they don't share.  Marks are bits
 * by block number.  Then every node the store can find by hash that isn't
 * marked is reclaimed: dropped from the map, its block reused once every
 * transaction running now has ended (a reader may still hold a root it
 * took before).
 *
 * Roots a transaction holds only in its own memory (the trees it is
 * building, spilling or has prepared) belong to writers, who hold the
 * store lock, so collect() never runs while they exist.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/reclaim_marksweep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "catalog/pg_authid.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(prolly_gc);

typedef struct Marks
{
	uint8	   *bits;
	BlockNumber nbits;
	uint64		nmarked;
	ProllyRootEnv root_env;
	const ProllyRootLayoutRoutine *root_layout;
	ProllyHash *garbage;
	BlockNumber *garbage_blocks;
	int64		ngarbage;
	int64		maxgarbage;
} Marks;

static bool
marked(const Marks *m, BlockNumber blk)
{
	return blk < m->nbits && (m->bits[blk / 8] & (1 << (blk % 8))) != 0;
}

static void
mark(Marks *m, BlockNumber blk)
{
	if (blk >= m->nbits)
	{
		BlockNumber nbits = Max(blk + 1, m->nbits * 2);

		Size		oldsize = (m->nbits + 7) / 8;
		Size		newsize = (nbits + 7) / 8;

		m->bits = repalloc_huge(m->bits, newsize);
		memset(m->bits + oldsize, 0, newsize - oldsize);
		m->nbits = nbits;
	}
	m->bits[blk / 8] |= (uint8) (1 << (blk % 8));
}

static bool
mark_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Marks	   *m = arg;
	BlockNumber blk = node->location.blkno;

	if (!BlockNumberIsValid(blk))
		elog(ERROR, "prolly reclamation visited a node with no block");
	if (marked(m, blk))
		return false;
	mark(m, blk);
	m->nmarked++;
	return true;
}

static void
mark_root(ProllyBranchId branch, const ProllyNodeRef *root, void *arg)
{
	Marks	   *m = arg;

	CHECK_FOR_INTERRUPTS();
	if (branch == PROLLY_REFS_BRANCH)
	{
		/* Not a working set: the refs, and the commits they keep. */
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		PROLLY_CALL(dc.refs, walk, root, mark_node, m);
	}
	else
		PROLLY_CALL(m->root_layout, walk, &m->root_env, root, mark_node, m);
}

static void
sweep_node(const ProllyHash *hash, BlockNumber block, void *arg)
{
	Marks	   *m = arg;

	if (marked(m, block))
		return;
	if (m->ngarbage == m->maxgarbage)
	{
		m->maxgarbage = Max(1024, m->maxgarbage * 2);
		m->garbage = repalloc_huge(m->garbage, sizeof(ProllyHash) * m->maxgarbage);
		m->garbage_blocks = repalloc_huge(m->garbage_blocks,
										  sizeof(BlockNumber) * m->maxgarbage);
	}
	m->garbage[m->ngarbage] = *hash;
	m->garbage_blocks[m->ngarbage] = block;
	m->ngarbage++;
}

static void
marksweep_collect(ProllyGcResult *result)
{
	ProllyComponents c;
	ProllyNodeStore *store;
	ProllyTreeEnv env;
	Marks		m;

	prolly_components_for_engine(&prolly_engine_d1.info, &c);
	(void) LockProllyStore(ExclusiveLock, false);
	store = PROLLY_CALL(c.node_store, open, NULL);

	memset(&env, 0, sizeof(env));
	env.node_format = c.node_format;
	env.chunker = c.chunker;
	env.hash = c.hash;
	env.node_store = c.node_store;
	env.store = store;
	env.log = c.log;
	memset(&m, 0, sizeof(m));
	m.nbits = 8192;
	m.bits = palloc0(m.nbits / 8);
	m.maxgarbage = 1024;
	m.garbage = palloc(sizeof(ProllyHash) * m.maxgarbage);
	m.garbage_blocks = palloc(sizeof(BlockNumber) * m.maxgarbage);
	m.root_layout = c.root_layout;
	m.root_env.tree = c.tree;
	m.root_env.tree_env = &env;

	/*
	 * The layout's own trees order keys by memcmp; relation trees are
	 * walked, which needs no order.
	 */
	env.compare = prolly_bytes_compare;
	PROLLY_CALL(c.commit, live_roots, mark_root, &m);

	PROLLY_CALL(c.node_store, scan, store, sweep_node, &m);
	for (int64 i = 0; i < m.ngarbage; i++)
	{
		CHECK_FOR_INTERRUPTS();
		PROLLY_CALL(c.node_store, reclaim, store, &m.garbage[i],
					m.garbage_blocks[i]);
	}
	PROLLY_CALL(c.node_store, close, store);

	result->nodes_kept = m.nmarked;
	result->nodes_freed = m.ngarbage;
	pfree(m.bits);
	pfree(m.garbage);
	pfree(m.garbage_blocks);
}

const ProllyReclaimRoutine prolly_reclaim_mark_sweep = {
	.info = {
		.iface = PROLLY_IFACE_RECLAMATION,
		.impl_id = 1,
		.format_version = 1,
		.name = "mark_sweep",
		.description = "mark from every live root, sweep the node store's "
		"unreached nodes (prolly_gc)",
	},
	.collect = marksweep_collect,
};

/*
 * prolly_gc(): reclaim the cluster's unreachable prolly nodes.  Writers of
 * prolly tables wait while it runs.
 */
Datum
prolly_gc(PG_FUNCTION_ARGS)
{
	ProllyComponents c;
	ProllyGcResult result;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	if (!has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reclaim prolly storage"),
				 errdetail("Only roles with privileges of the \"%s\" role may run prolly_gc().",
						   "pg_maintain")));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	prolly_components_for_engine(&prolly_engine_d1.info, &c);
	PROLLY_CALL(c.reclamation, collect, &result);
	values[0] = Int64GetDatum((int64) result.nodes_kept);
	values[1] = Int64GetDatum((int64) result.nodes_freed);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
