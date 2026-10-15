/*--------------------------------------------------------------------------
 *
 * test_prolly.c
 *		Conformance suites for the prolly storage interfaces.
 *
 * Every interface (src/include/access/prolly_*.h) has a suite of named
 * cases here, and every registered implementation of that interface is run
 * against it.  A case lists the callbacks it needs; if the implementation
 * lacks any, the result is "not implemented".  A case whose checks aren't
 * written yet reports "no checks yet" once its callbacks exist.  Cases get
 * their checks as the first real implementations are built.
 *
 * Every interface also gets two generic cases: "identity" (the
 * implementation's ProllyImplInfo is sane and the registry finds it) and
 * "complete" (every callback is provided).
 *
 * To show that the suites can pass and fail, _PG_init registers two
 * content-hash implementations of its own: test_fnv, which is correct, and
 * test_broken, which ignores all but the first update() call.  (Real
 * implementations register from shared_preload_libraries; registering at
 * CREATE EXTENSION affects only this backend, which is all a test needs.)
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/test_prolly.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_commit.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "access/prolly_config.h"
#include "common/hashfn.h"
#include "access/relation.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"
#include "test_prolly.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(prolly_implementations);
PG_FUNCTION_INFO_V1(prolly_conformance);

typedef void (*ConformanceBody) (const ProllyImplInfo *impl);

typedef struct ConformanceCase
{
	ProllyInterface iface;
	const char *name;
	const char *requires;		/* space-separated callback names */
	ConformanceBody body;		/* NULL: checks not written yet */
} ConformanceCase;

static void check_identity(const ProllyImplInfo *impl);
static void hash_deterministic(const ProllyImplInfo *impl);
static void hash_incremental(const ProllyImplInfo *impl);

/*
 * The suites.  Case names describe the contract they will check, taken from
 * the interface headers' contracts.
 */
static const ConformanceCase cases[] = {
	/* engine */
	{PROLLY_IFACE_ENGINE, "create_and_truncate",
	"tree_create tree_truncate put cursor_open cursor_seek cursor_next cursor_close",
	engine_create_and_truncate},
	{PROLLY_IFACE_ENGINE, "read_empty_tree",
	"tree_create roots_acquire roots_release cursor_open cursor_seek cursor_next cursor_close get",
	engine_read_empty_tree},
	{PROLLY_IFACE_ENGINE, "read_own_writes",
	"tree_create put remove get cursor_open cursor_seek cursor_next cursor_close",
	engine_read_own_writes},
	{PROLLY_IFACE_ENGINE, "seek_sees_later_writes",
	"tree_create put tree_reset cursor_open cursor_seek cursor_next cursor_close",
	engine_seek_sees_later_writes},
	{PROLLY_IFACE_ENGINE, "subxact_abort_discards_writes",
	"tree_create put remove subxact_abort xact_abort cursor_open cursor_next",
	engine_subxact_abort_discards_writes},
	{PROLLY_IFACE_ENGINE, "commit_publishes_roots",
	"put xact_precommit xact_commit roots_acquire", NULL},
	{PROLLY_IFACE_ENGINE, "count_range_matches_scan",
	"tree_create put count_range cursor_next", engine_count_range_matches_scan},
	{PROLLY_IFACE_ENGINE, "canonical_root_independent_of_history",
	"put remove xact_precommit canonical_root", NULL},
	{PROLLY_IFACE_ENGINE, "export_chunks_closed_under_reachability",
	"canonical_root export_chunks", NULL},

	/* coordination */
	{PROLLY_IFACE_COORDINATION, "lock_conflict_matrix", "lock_row", NULL},
	{PROLLY_IFACE_COORDINATION, "wait_policies", "lock_row", NULL},
	{PROLLY_IFACE_COORDINATION, "speculative_insertion",
	"insert_intent insert_intent_complete", NULL},
	{PROLLY_IFACE_COORDINATION, "commit_markers_until_horizon",
	"lock_row xact_commit written_since prune_markers", NULL},
	{PROLLY_IFACE_COORDINATION, "abort_releases_locks",
	"lock_row xact_abort subxact_abort", NULL},
	{PROLLY_IFACE_COORDINATION, "predicate_locks", "predicate_lock_range", NULL},

	/* node format */
	{PROLLY_IFACE_NODE_FORMAT, "items_round_trip",
	"page_init item_append item_hint item_count item_get",
	nodeformat_items_round_trip},
	{PROLLY_IFACE_NODE_FORMAT, "level_round_trip", "page_init page_level item_count", nodeformat_level_round_trip},
	{PROLLY_IFACE_NODE_FORMAT, "space_accounting",
	"page_init item_space free_space item_append", nodeformat_space_accounting},
	{PROLLY_IFACE_NODE_FORMAT, "canonical_bytes_depend_only_on_items",
	"page_init item_append canonical_bytes special_space",
	nodeformat_canonical_bytes},
	{PROLLY_IFACE_NODE_FORMAT, "page_check_detects_corruption",
	"page_init item_append page_check", nodeformat_page_check},

	/* chunker */
	{PROLLY_IFACE_CHUNKER, "deterministic", "begin add end", chunker_deterministic},
	{PROLLY_IFACE_CHUNKER, "forced_split_respects_capacity", "begin add end",
	chunker_forced_split},
	{PROLLY_IFACE_CHUNKER, "reset_equals_begin", "begin add reset end",
	chunker_reset_equals_begin},

	/* content hash */
	{PROLLY_IFACE_CONTENT_HASH, "deterministic", "begin update finish",
	hash_deterministic},
	{PROLLY_IFACE_CONTENT_HASH, "incremental_equals_one_shot",
	"begin update finish", hash_incremental},

	/* key encoder */
	{PROLLY_IFACE_KEY_ENCODER, "order_matches_opclass",
	"supports begin encode compare end", keyenc_order_matches_opclass},
	{PROLLY_IFACE_KEY_ENCODER, "desc_and_nulls_ordering",
	"supports begin encode compare end", keyenc_desc_and_nulls},
	{PROLLY_IFACE_KEY_ENCODER, "decode_round_trip", "begin encode decode end",
	keyenc_decode_round_trip},
	{PROLLY_IFACE_KEY_ENCODER, "self_delimiting_prefixes",
	"begin encode prefix_length end", keyenc_prefix_lengths},
	{PROLLY_IFACE_KEY_ENCODER, "equal_bytes_name_keys",
	"begin encode compare equal_bytes end", keyenc_equal_bytes},

	/* value encoder */
	{PROLLY_IFACE_VALUE_ENCODER, "round_trip", "begin encode decode end",
	valenc_round_trip},
	{PROLLY_IFACE_VALUE_ENCODER, "platform_independent", "begin encode end",
	valenc_platform_independent},
	{PROLLY_IFACE_VALUE_ENCODER, "independent_of_history", "begin encode end",
	valenc_independent_of_history},
	{PROLLY_IFACE_VALUE_ENCODER, "independent_of_session", "begin encode end",
	valenc_independent_of_session},
	{PROLLY_IFACE_VALUE_ENCODER, "missing_values", "begin encode decode end",
	valenc_missing_values},
	{PROLLY_IFACE_VALUE_ENCODER, "decodes_earlier_layouts",
	"begin layout begin_layout reads_layout encode decode end",
	valenc_decodes_earlier_layouts},

	/* node store */
	{PROLLY_IFACE_NODE_STORE, "new_publish_read",
	"create open new_node publish read contains close",
	nodestore_new_publish_read},
	{PROLLY_IFACE_NODE_STORE, "many_nodes_findable",
	"create open new_node publish read contains close", nodestore_many_nodes},
	{PROLLY_IFACE_NODE_STORE, "release_frees",
	"create open new_node publish release contains close",
	nodestore_release_frees},

	/* allocator */
	{PROLLY_IFACE_ALLOCATOR, "no_overlapping_extents",
	"open allocate free close", NULL},

	/* reclamation */
	{PROLLY_IFACE_RECLAMATION, "reachable_never_freed",
	"node_created node_dropped root_retired run", NULL},
	{PROLLY_IFACE_RECLAMATION, "in_place_only_when_private",
	"may_modify_in_place", NULL},

	/* log */
	{PROLLY_IFACE_LOG, "commit_record", "log_commit", NULL},
	{PROLLY_IFACE_LOG, "redo_restores_committed_root",
	"log_node log_mutation log_commit redo startup cleanup", NULL},
	{PROLLY_IFACE_LOG, "describes_records", "desc identify", NULL},

	/* commit pipeline */
	{PROLLY_IFACE_COMMIT_PIPELINE, "publishes_in_enqueue_order",
	"branch_lock tip enqueue branch_unlock publish abandon head snapshot_head",
	commit_publishes_in_enqueue_order},
	{PROLLY_IFACE_COMMIT_PIPELINE, "survives_crash_and_replays",
	"enqueue publish checkpoint redo", NULL},
	{PROLLY_IFACE_COMMIT_PIPELINE, "copies_and_drops_databases",
	"copy_database drop_database usage head checkpoint redo", NULL},

	/* row identity */
	{PROLLY_IFACE_ROW_IDENTITY, "assign_then_resolve",
	"tid_assign scan_begin tid_find scan_end tid_resolve", rowid_assign_then_resolve},
	{PROLLY_IFACE_ROW_IDENTITY, "rename_keeps_tid",
	"tid_assign tid_rename tid_resolve scan_begin tid_find scan_end",
	rowid_rename_keeps_tid},
	{PROLLY_IFACE_ROW_IDENTITY, "find_sees_later_assign",
	"scan_begin tid_find tid_assign scan_end", rowid_find_sees_later_assign},
	{PROLLY_IFACE_ROW_IDENTITY, "renew_makes_new_version",
	"tid_assign tid_renew tid_resolve scan_begin tid_find scan_end",
	rowid_renew},
	{PROLLY_IFACE_ROW_IDENTITY, "never_reused",
	"tid_assign tid_retire tid_resolve scan_begin tid_find scan_end",
	rowid_never_reused},
	{PROLLY_IFACE_ROW_IDENTITY, "forget", "tid_assign tid_resolve tid_forget", NULL},

	/* table keying */
	{PROLLY_IFACE_TABLE_KEYING, "round_trip",
	"open keyless encode_row make_value parse_value decode_row close",
	keying_round_trip},
	{PROLLY_IFACE_TABLE_KEYING, "keys_and_content",
	"open encode_row content_hash compare close", keying_keys_and_content},
	{PROLLY_IFACE_TABLE_KEYING, "description_reopens",
	"open describe keyless encode_row close", keying_description_reopens},
	{PROLLY_IFACE_TABLE_KEYING, "reads_after_key_column_dropped",
	"open describe encode_row compare decode_row close", keying_reads_dropped_key},
	{PROLLY_IFACE_TABLE_KEYING, "owns_its_memory",
	"open encode_row decode_row close", keying_owns_its_memory},
	{PROLLY_IFACE_TABLE_KEYING, "cannot_compare_after_key_type_dropped",
	"open describe can_compare close", keying_compare_after_type_dropped},

	/* index keying */
	{PROLLY_IFACE_INDEX_KEYING, "entries_and_bounds",
	"open describe nkeys encode_entry decode_entry encode_bound compare columns_equal close",
	ixkeying_entries},

	/* refs */
	{PROLLY_IFACE_REFS, "branch_lifecycle",
	"branch_create branch_lookup branch_delete", NULL},
	{PROLLY_IFACE_REFS, "commit_then_resolve", "commit resolve commit_get", NULL},
	{PROLLY_IFACE_REFS, "merge_base", "commit merge_base", NULL},

	/* transfer */
	{PROLLY_IFACE_TRANSFER, "clone", "clone", NULL},
	{PROLLY_IFACE_TRANSFER, "fetch_then_push", "fetch push", NULL},

	/* diff and merge */
	{PROLLY_IFACE_DIFF_MERGE, "diff_reports_each_change_once", "diff", NULL},
	{PROLLY_IFACE_DIFF_MERGE, "merge", "merge", NULL},

	/* tree */
	{PROLLY_IFACE_TREE, "build_and_get",
	"build_begin build_add build_finish get cursor_open cursor_seek cursor_next cursor_close count",
	tree_build_and_get},
	{PROLLY_IFACE_TREE, "cursor_scans",
	"build_begin build_add build_finish cursor_open cursor_seek cursor_next cursor_close count",
	tree_cursor_scans},
	{PROLLY_IFACE_TREE, "apply_matches_reference",
	"build_begin build_add build_finish apply cursor_open cursor_seek cursor_next cursor_close count",
	tree_apply_matches_reference},
	{PROLLY_IFACE_TREE, "apply_past_the_end",
	"build_begin build_add build_finish apply cursor_open cursor_seek cursor_next cursor_close count",
	tree_apply_past_the_end},
	{PROLLY_IFACE_TREE, "history_independent",
	"build_begin build_add build_finish apply cursor_open cursor_seek cursor_next cursor_close count",
	tree_history_independent},
	{PROLLY_IFACE_TREE, "edits_share_nodes",
	"build_begin build_add build_finish apply walk", tree_edits_share_nodes},
	{PROLLY_IFACE_TREE, "diff_reports_changes",
	"build_begin build_add build_finish apply diff", tree_diff_reports_changes},
	{PROLLY_IFACE_TREE, "equal_keys_take_new_bytes",
	"build_begin build_add build_finish apply cursor_open cursor_seek cursor_next cursor_close diff",
	tree_equal_keys_take_new_bytes},
	{PROLLY_IFACE_TREE, "counts_nodes",
	"build_begin build_add build_finish walk", tree_counts_nodes},
	{PROLLY_IFACE_TREE, "diff_reads_changed_paths",
	"build_begin build_add build_finish apply walk diff",
	tree_diff_reads_changed_paths},
	{PROLLY_IFACE_TREE, "diff_across_heights",
	"build_begin build_add build_finish walk diff", tree_diff_across_heights},
	{PROLLY_IFACE_TREE, "seeks_in_order",
	"build_begin build_add build_finish walk cursor_open cursor_seek cursor_next cursor_close",
	tree_seeks_in_order},
	{PROLLY_IFACE_TREE, "nodes_are_valid",
	"build_begin build_add build_finish walk", tree_nodes_are_valid},
	{PROLLY_IFACE_TREE, "large_values",
	"build_begin build_add build_finish apply get cursor_open cursor_seek cursor_next cursor_close walk diff",
	tree_large_values},

	/* root layout */
	{PROLLY_IFACE_ROOT_LAYOUT, "empty", "empty subroot lookup scan apply",
	rootlayout_empty},
	{PROLLY_IFACE_ROOT_LAYOUT, "apply_matches_reference",
	"empty apply lookup scan", rootlayout_apply_matches_reference},
	{PROLLY_IFACE_ROOT_LAYOUT, "history_independent",
	"empty apply lookup scan", rootlayout_history_independent},
	{PROLLY_IFACE_ROOT_LAYOUT, "subroots_independent",
	"empty apply subroot", rootlayout_subroots_independent},
	{PROLLY_IFACE_ROOT_LAYOUT, "diff_reports_changes",
	"empty apply diff", rootlayout_diff_reports_changes},
	{PROLLY_IFACE_ROOT_LAYOUT, "apply_rejects_invalid_refs",
	"empty apply lookup", rootlayout_apply_rejects_invalid_refs},
	{PROLLY_IFACE_ROOT_LAYOUT, "walk_reaches_trees",
	"empty apply subroot walk", rootlayout_walk_reaches_trees},
};

/* ------------------------------------------------------------------------
 * Checks
 * ------------------------------------------------------------------------
 */

static void
check_identity(const ProllyImplInfo *impl)
{
	if (impl->name == NULL || impl->name[0] == '\0')
		elog(ERROR, "implementation has no name");
	if (impl->description == NULL)
		elog(ERROR, "implementation \"%s\" has no description", impl->name);
	if (prolly_lookup_impl(impl->iface, impl->name) != impl)
		elog(ERROR, "registry does not find \"%s\" by name", impl->name);
	if (prolly_lookup_impl_id(impl->iface, impl->impl_id) != impl)
		elog(ERROR, "registry does not find \"%s\" by impl_id %u",
			 impl->name, (unsigned) impl->impl_id);
}

static void
hash_once(const ProllyHashRoutine *hash, const uint8 *data, Size len,
		  Size piece, ProllyHash *out)
{
	ProllyHashCtx *ctx = PROLLY_CALL(hash, begin, CurrentMemoryContext);

	for (Size off = 0; off < len; off += piece)
		PROLLY_CALL(hash, update, ctx, data + off, Min(piece, len - off));
	PROLLY_CALL(hash, finish, ctx, out);

	if (out->len == 0 || out->len > PROLLY_HASH_MAX_LEN)
		elog(ERROR, "digest length %d is out of range", out->len);
}

static bool
hash_equal(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static void
hash_deterministic(const ProllyImplInfo *impl)
{
	const ProllyHashRoutine *hash = (const ProllyHashRoutine *) impl;
	static const uint8 data[] = "prolly conformance";
	ProllyHash	a;
	ProllyHash	b;
	ProllyHash	c;

	hash_once(hash, data, sizeof(data), sizeof(data), &a);
	hash_once(hash, data, sizeof(data), sizeof(data), &b);
	if (!hash_equal(&a, &b))
		elog(ERROR, "same input gave different digests");

	hash_once(hash, data, sizeof(data) - 1, sizeof(data), &c);
	if (hash_equal(&a, &c))
		elog(ERROR, "different inputs gave the same digest");
	if (a.len != c.len)
		elog(ERROR, "digest length varies with input");
}

static void
hash_incremental(const ProllyImplInfo *impl)
{
	const ProllyHashRoutine *hash = (const ProllyHashRoutine *) impl;
	uint8		data[1000];
	ProllyHash	whole;
	ProllyHash	pieces;

	for (int i = 0; i < lengthof(data); i++)
		data[i] = (uint8) (i * 31 + 7);

	hash_once(hash, data, sizeof(data), sizeof(data), &whole);
	hash_once(hash, data, sizeof(data), 7, &pieces);
	if (!hash_equal(&whole, &pieces))
		elog(ERROR, "hashing in pieces differs from hashing at once");
}

/* ------------------------------------------------------------------------
 * Test-only content hashes: 64-bit FNV-1a, correct and broken
 * ------------------------------------------------------------------------
 */

struct ProllyHashCtx
{
	uint64		state;
	int			nupdates;
	bool		broken;
};

static ProllyHashCtx *
fnv_begin(MemoryContext mcxt)
{
	ProllyHashCtx *ctx = MemoryContextAllocZero(mcxt, sizeof(ProllyHashCtx));

	ctx->state = UINT64CONST(0xcbf29ce484222325);
	return ctx;
}

static ProllyHashCtx *
broken_begin(MemoryContext mcxt)
{
	ProllyHashCtx *ctx = fnv_begin(mcxt);

	ctx->broken = true;
	return ctx;
}

static void
fnv_update(ProllyHashCtx *ctx, const uint8 *data, Size len)
{
	if (ctx->broken && ctx->nupdates++ > 0)
		return;
	for (Size i = 0; i < len; i++)
	{
		ctx->state ^= data[i];
		ctx->state *= UINT64CONST(0x100000001b3);
	}
}

static void
fnv_finish(ProllyHashCtx *ctx, ProllyHash *out)
{
	memset(out, 0, sizeof(*out));
	out->len = sizeof(uint64);
	for (int i = 0; i < sizeof(uint64); i++)
		out->bytes[i] = (uint8) (ctx->state >> (8 * i));
	pfree(ctx);
}

static const ProllyHashRoutine test_fnv_hash = {
	.info = {
		.iface = PROLLY_IFACE_CONTENT_HASH,
		.impl_id = 9001,
		.name = "test_fnv",
		.description = "test only: 64-bit FNV-1a",
	},
	.begin = fnv_begin,
	.update = fnv_update,
	.finish = fnv_finish,
};

static const ProllyHashRoutine test_broken_hash = {
	.info = {
		.iface = PROLLY_IFACE_CONTENT_HASH,
		.impl_id = 9002,
		.name = "test_broken",
		.description = "test only: ignores all but the first update",
	},
	.begin = broken_begin,
	.update = fnv_update,
	.finish = fnv_finish,
};

void
_PG_init(void)
{
	prolly_register_impl(&test_fnv_hash.info);
	prolly_register_impl(&test_broken_hash.info);
}

/* ------------------------------------------------------------------------
 * Running cases
 * ------------------------------------------------------------------------
 */

static int
callback_number(ProllyInterface iface, const char *name)
{
	for (int i = 0; i < prolly_interface_ncallbacks(iface); i++)
	{
		if (strcmp(prolly_interface_callback_name(iface, i), name) == 0)
			return i;
	}
	elog(ERROR, "conformance case names unknown %s callback \"%s\"",
		 prolly_interface_name(iface), name);
	return -1;					/* keep compiler quiet */
}

/* Missing callbacks among those listed in requires, or "" if none. */
static char *
missing_callbacks(const ProllyImplInfo *impl, const char *requires)
{
	StringInfoData missing;
	char	   *list = pstrdup(requires);
	char	   *saveptr;

	initStringInfo(&missing);
	for (char *tok = strtok_r(list, " ", &saveptr); tok != NULL;
		 tok = strtok_r(NULL, " ", &saveptr))
	{
		if (!prolly_impl_has_callback(impl, callback_number(impl->iface, tok)))
			appendStringInfo(&missing, "%s%s", missing.len > 0 ? ", " : "", tok);
	}
	return missing.data;
}

/*
 * Run body against impl in a subtransaction, so that a failing check (or a
 * "not implemented" error) is reported as a result instead of aborting the
 * suite.
 */
static char *
run_body(ConformanceBody body, const ProllyImplInfo *impl)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	char	   *result;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcxt);

	PG_TRY();
	{
		body(impl);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
		result = pstrdup("pass");
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;

		if (edata->sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED)
			result = psprintf("not implemented (%s)", edata->message);
		else
			result = psprintf("fail: %s", edata->message);
	}
	PG_END_TRY();

	return result;
}

static void
emit(ReturnSetInfo *rsinfo, const ProllyImplInfo *impl, const char *test,
	 const char *result)
{
	Datum		values[4];
	bool		nulls[4] = {0};

	values[0] = CStringGetTextDatum(prolly_interface_name(impl->iface));
	values[1] = CStringGetTextDatum(impl->name);
	values[2] = CStringGetTextDatum(test);
	values[3] = CStringGetTextDatum(result);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

static void
run_suite(ReturnSetInfo *rsinfo, const ProllyImplInfo *impl)
{
	int			ncallbacks = prolly_interface_ncallbacks(impl->iface);
	int			nmissing = 0;

	emit(rsinfo, impl, "identity", run_body(check_identity, impl));

	for (int i = 0; i < lengthof(cases); i++)
	{
		const ConformanceCase *c = &cases[i];
		char	   *missing;

		if (c->iface != impl->iface)
			continue;

		missing = missing_callbacks(impl, c->requires);
		if (missing[0] != '\0')
			emit(rsinfo, impl, c->name,
				 psprintf("not implemented (missing %s)", missing));
		else if (c->body == NULL)
			emit(rsinfo, impl, c->name, "no checks yet");
		else
			emit(rsinfo, impl, c->name, run_body(c->body, impl));
	}

	for (int i = 0; i < ncallbacks; i++)
	{
		if (!prolly_impl_has_callback(impl, i))
			nmissing++;
	}
	emit(rsinfo, impl, "complete",
		 nmissing == 0 ? "pass" :
		 psprintf("not implemented (%d of %d callbacks missing)",
				  nmissing, ncallbacks));
}

/* ------------------------------------------------------------------------
 * SQL functions
 * ------------------------------------------------------------------------
 */

Datum
prolly_implementations(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	for (int iface = PROLLY_IFACE_FIRST; iface <= PROLLY_IFACE_LAST; iface++)
	{
		ListCell   *lc;

		foreach(lc, prolly_list_impls((ProllyInterface) iface))
		{
			const ProllyImplInfo *impl = lfirst(lc);
			Datum		values[5];
			bool		nulls[5] = {0};

			values[0] = CStringGetTextDatum(prolly_interface_name(impl->iface));
			values[1] = CStringGetTextDatum(impl->name);
			values[2] = Int32GetDatum(impl->impl_id);
			values[3] = Int32GetDatum(impl->format_version);
			values[4] = CStringGetTextDatum(impl->description);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}
	return (Datum) 0;
}

Datum
prolly_conformance(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *only_iface = PG_ARGISNULL(0) ? NULL :
		text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *only_impl = PG_ARGISNULL(1) ? NULL :
		text_to_cstring(PG_GETARG_TEXT_PP(1));
	ProllyInterface wanted = 0;

	if (only_iface != NULL && !prolly_interface_from_name(only_iface, &wanted))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown prolly interface \"%s\"", only_iface)));

	InitMaterializedSRF(fcinfo, 0);

	for (int iface = PROLLY_IFACE_FIRST; iface <= PROLLY_IFACE_LAST; iface++)
	{
		ListCell   *lc;

		if (only_iface != NULL && iface != wanted)
			continue;

		foreach(lc, prolly_list_impls((ProllyInterface) iface))
		{
			const ProllyImplInfo *impl = lfirst(lc);

			if (only_impl == NULL || strcmp(only_impl, impl->name) == 0)
				run_suite(rsinfo, impl);
		}
	}
	return (Datum) 0;
}

/*
 * prolly_commit_usage(implementation): how many heads and pending roots a
 * commit pipeline holds.
 */
PG_FUNCTION_INFO_V1(prolly_commit_usage);

Datum
prolly_commit_usage(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const ProllyCommitRoutine *r = prolly_commit_lookup(name);
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};
	int			nheads;
	int			npending;

	if (r == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no commit pipeline implementation \"%s\"", name)));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PROLLY_CALL(r, usage, &nheads, &npending);
	values[0] = Int32GetDatum(nheads);
	values[1] = Int32GetDatum(npending);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * prolly_fail_next_commit(): the backend's next transaction (not the one
 * calling this) raises an error just before it commits, for tests of what
 * an aborted statement that can't run in a transaction block leaves behind.
 */
PG_FUNCTION_INFO_V1(prolly_fail_next_commit);

static LocalTransactionId fail_after_lxid = InvalidLocalTransactionId;
static bool fail_callback_registered = false;

static void
fail_commit_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_PRE_COMMIT ||
		fail_after_lxid == InvalidLocalTransactionId ||
		MyProc->vxid.lxid == fail_after_lxid)
		return;
	fail_after_lxid = InvalidLocalTransactionId;
	ereport(ERROR,
			(errmsg("commit failed by prolly_fail_next_commit()")));
}

Datum
prolly_fail_next_commit(PG_FUNCTION_ARGS)
{
	if (!fail_callback_registered)
	{
		RegisterXactCallback(fail_commit_callback, NULL);
		fail_callback_registered = true;
	}
	fail_after_lxid = MyProc->vxid.lxid;
	PG_RETURN_VOID();
}

/*
 * prolly_commit_test_enqueue(implementation, branch): commit a made-up
 * root to a branch of the current database through a commit pipeline, as
 * part of the current transaction: published if it commits, abandoned if
 * it aborts.  For filling a pipeline's tables in tests; nothing reads the
 * branches it writes, so use branches other than the main one.
 */
PG_FUNCTION_INFO_V1(prolly_commit_test_enqueue);

#define MAX_TEST_TICKETS 4096

static const ProllyCommitRoutine *test_tickets_routine = NULL;
static ProllyCommitTicket test_tickets[MAX_TEST_TICKETS];
static int	ntest_tickets = 0;
static bool test_tickets_registered = false;

static void
test_tickets_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
		return;
	for (int i = 0; i < ntest_tickets; i++)
	{
		if (event == XACT_EVENT_COMMIT)
			PROLLY_CALL(test_tickets_routine, publish, test_tickets[i]);
		else
			PROLLY_CALL(test_tickets_routine, abandon, test_tickets[i]);
	}
	ntest_tickets = 0;
}

Datum
prolly_commit_test_enqueue(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		branch = PG_GETARG_INT64(1);
	const ProllyCommitRoutine *r = prolly_commit_lookup(name);
	ProllyNodeRef base;
	ProllyNodeRef root;

	if (r == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no commit pipeline implementation \"%s\"", name)));
	if (branch <= PROLLY_MAIN_BRANCH || branch > PG_UINT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("test commits go to branches after the main one")));
	if (ntest_tickets > 0 && test_tickets_routine != r)
		elog(ERROR, "test commits to two pipelines in one transaction");
	if (ntest_tickets >= MAX_TEST_TICKETS)
		elog(ERROR, "too many test commits in one transaction");
	if (!test_tickets_registered)
	{
		RegisterXactCallback(test_tickets_callback, NULL);
		test_tickets_registered = true;
	}

	memset(&root, 0, sizeof(root));
	root.location.blkno = InvalidBlockNumber;
	root.hash.len = 32;
	for (int i = 0; i < 32; i++)
		root.hash.bytes[i] = (uint8) ((branch >> ((i % 8) * 8)) ^ (i * 37) ^
									  MyProc->vxid.lxid);

	PROLLY_CALL(r, branch_lock, (ProllyBranchId) branch);
	(void) PROLLY_CALL(r, tip, (ProllyBranchId) branch, &base);
	test_tickets_routine = r;
	test_tickets[ntest_tickets++] = PROLLY_CALL(r, enqueue,
												(ProllyBranchId) branch, &root,
												GetCurrentTransactionId());
	PROLLY_CALL(r, branch_unlock, (ProllyBranchId) branch);
	PG_RETURN_VOID();
}

/*
 * prolly_tree_digest(rel): the number of items in rel's tree (a prolly
 * table's data tree, or a prolly index's), and a digest of them, as the
 * current transaction sees it.  Two trees with the same items have the
 * same digest, however they were written.
 */
PG_FUNCTION_INFO_V1(prolly_tree_digest);

Datum
prolly_tree_digest(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel = relation_open(relid, AccessShareLock);
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyCursor *cur;
	ProllyKey	key;
	ProllyValue value;
	uint64		n = 0;
	uint64		h = 0;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	cur = PROLLY_CALL(c.engine, cursor_open, roots, rel, PROLLY_TREE_DATA,
					  InvalidCommandId);
	PROLLY_CALL(c.engine, cursor_seek, cur, NULL, ForwardScanDirection);
	while (PROLLY_CALL(c.engine, cursor_next, cur, &key, &value))
	{
		h = hash_combine64(h, hash_bytes_extended(key.data, key.len, n));
		h = hash_combine64(h, hash_bytes_extended(value.data, value.len, n));
		n++;
	}
	PROLLY_CALL(c.engine, cursor_close, cur);
	PROLLY_CALL(c.engine, roots_release, roots);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(UINT64_FORMAT ":%016" PRIx64, n, h)));
}
