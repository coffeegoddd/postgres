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

#include "access/prolly_config.h"
#include "access/prolly_remote.h"
#include "access/xact.h"
#include "utils/wait_event.h"
#include "storage/latch.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
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
static void hash_buckets_spread(const ProllyImplInfo *impl);

/*
 * The suites.  Case names describe the contract they will check, taken from
 * the interface headers' contracts.
 */
static const ConformanceCase cases[] = {
	/* chunker */
	{PROLLY_IFACE_CHUNKER, "deterministic", "begin add end", chunker_deterministic},
	{PROLLY_IFACE_CHUNKER, "forced_split_respects_capacity", "begin add end",
	chunker_forced_split},

	/* chunk node format */
	{PROLLY_IFACE_CHUNK_FORMAT, "items_round_trip",
	"encode open item_get item_space header_size", chunkformat_items_round_trip},
	{PROLLY_IFACE_CHUNK_FORMAT, "rejects_other_bytes", "encode open",
	chunkformat_rejects_other_bytes},

	/* table file format */
	{PROLLY_IFACE_TABLE_FILE, "round_trip",
	"write_begin write_add write_finish open find decode count entry close",
	tablefile_round_trip},
	{PROLLY_IFACE_TABLE_FILE, "rejects_other_bytes",
	"write_begin write_add write_finish open entry decode close",
	tablefile_rejects_other_bytes},
	{PROLLY_IFACE_TABLE_FILE, "shared_prefixes",
	"write_begin write_add write_finish open find decode close",
	tablefile_shared_prefixes},

	/* content hash */
	{PROLLY_IFACE_CONTENT_HASH, "deterministic", "begin update finish",
	hash_deterministic},
	{PROLLY_IFACE_CONTENT_HASH, "incremental_equals_one_shot",
	"begin update finish", hash_incremental},
	{PROLLY_IFACE_CONTENT_HASH, "buckets_spread", "begin update finish",
	hash_buckets_spread},

	/* key encoder */
	{PROLLY_IFACE_KEY_ENCODER, "order_matches_opclass",
	"supports begin encode compare end", keyenc_order_matches_opclass},
	{PROLLY_IFACE_KEY_ENCODER, "desc_and_nulls_ordering",
	"supports begin encode compare end", keyenc_desc_and_nulls},

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
	"begin layout begin_layout encode decode end",
	valenc_decodes_earlier_layouts},
	{PROLLY_IFACE_VALUE_ENCODER, "long_values",
	"begin encode decode end set_values refs", valenc_long_values},
	{PROLLY_IFACE_VALUE_ENCODER, "kept_values",
	"begin encode end set_values decode_kept encode_kept", valenc_kept_values},
	{PROLLY_IFACE_VALUE_ENCODER, "selected_columns",
	"begin encode decode end set_values refs fields decode_field fields_alike field_compatible decode_columns project",
	valenc_selected_columns},
	{PROLLY_IFACE_VALUE_ENCODER, "assembled_rows",
	"begin encode decode end set_values fields encode_fields",
	valenc_assembled_rows},

	/* reclamation */
	{PROLLY_IFACE_RECLAMATION, "live_chunks_never_freed", "collect", NULL},
	{PROLLY_IFACE_RECLAMATION, "keeps_what_writers_reference", "collect", NULL},

	/* table keying */
	{PROLLY_IFACE_TABLE_KEYING, "round_trip",
	"open keyless encode_row make_value parse_value decode_row close",
	keying_round_trip},
	{PROLLY_IFACE_TABLE_KEYING, "keys_and_content",
	"open encode_row compare close encode_key", keying_keys_and_content},
	{PROLLY_IFACE_TABLE_KEYING, "owns_its_memory",
	"open encode_row decode_row close", keying_owns_its_memory},
	{PROLLY_IFACE_TABLE_KEYING, "row_shapes",
	"open close row_shape shape_extends", keying_row_shapes},
	{PROLLY_IFACE_TABLE_KEYING, "old_shapes",
	"open encode_row decode_row close row_shape open_shape", keying_old_shapes},
	{PROLLY_IFACE_TABLE_KEYING, "assembled_rows",
	"open encode_row close row_fields encode_row_fields", keying_assembled_rows},

	/* refs */
	{PROLLY_IFACE_REFS, "branch_lifecycle",
	"branch_create branch_lookup branch_delete", NULL},
	{PROLLY_IFACE_REFS, "commit_then_resolve", "commit resolve commit_get", NULL},
	{PROLLY_IFACE_REFS, "merge_base", "commit merge_base", NULL},

	/* transfer and remotes */
	{PROLLY_IFACE_TRANSFER, "receive_then_send", "receive send", NULL},
	{PROLLY_IFACE_REMOTE, "heads_and_chunks",
	"check_url open close format heads get has add_chunks move_head", NULL},

	/* diff and merge */
	{PROLLY_IFACE_DIFF_MERGE, "diff_reports_each_change_once", "diff", NULL},
	{PROLLY_IFACE_DIFF_MERGE, "merge", "merge", NULL},

	/* tree */
	{PROLLY_IFACE_TREE, "build_and_get",
	"build_begin build_add build_finish get cursor_open cursor_seek cursor_next cursor_close count",
	tree_build_and_get},
	{PROLLY_IFACE_TREE, "large_keys",
	"build_begin build_add build_finish apply get count key_max",
	tree_large_keys},
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
	{PROLLY_IFACE_TREE, "ranks",
	"build_begin build_add build_finish apply get count rank", tree_ranks},
	{PROLLY_IFACE_TREE, "diff_reads_changed_paths",
	"build_begin build_add build_finish apply walk diff",
	tree_diff_reads_changed_paths},
	{PROLLY_IFACE_TREE, "diff_stops",
	"build_begin build_add build_finish apply walk diff", tree_diff_stops},
	{PROLLY_IFACE_TREE, "diff_across_heights",
	"build_begin build_add build_finish walk diff", tree_diff_across_heights},
	{PROLLY_IFACE_TREE, "diff_unordered_matches",
	"build_begin build_add build_finish apply walk diff_unordered",
	tree_diff_unordered_matches},
	{PROLLY_IFACE_TREE, "diff_estimate_bounds",
	"build_begin build_add build_finish apply diff_estimate",
	tree_diff_estimate_bounds},
	{PROLLY_IFACE_TREE, "seeks_in_order",
	"build_begin build_add build_finish walk cursor_open cursor_seek cursor_next cursor_close",
	tree_seeks_in_order},
	{PROLLY_IFACE_TREE, "apply_reads_paths_once",
	"build_begin build_add build_finish apply get walk",
	tree_apply_reads_paths_once},
	{PROLLY_IFACE_TREE, "nodes_are_valid",
	"build_begin build_add build_finish walk", tree_nodes_are_valid},
	{PROLLY_IFACE_TREE, "large_values",
	"build_begin build_add build_finish apply get cursor_open cursor_seek cursor_next cursor_close walk diff value_write value_read value_reference value_root",
	tree_large_values},
	{PROLLY_IFACE_TREE, "held_references",
	"build_begin build_add build_finish apply get cursor_open cursor_seek cursor_next cursor_close walk diff diff_nodes value_write value_read",
	tree_held_references},
	{PROLLY_IFACE_TREE, "value_splices",
	"value_write value_read value_root value_splice", tree_value_splices},
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

/*
 * Digests' first four bytes spread evenly (prolly_hash_bucket()): 1024
 * inputs over 64 buckets by the low bits (dynahash's) and by the high
 * (dshash's), none empty and none past three times its share.
 */
static void
hash_buckets_spread(const ProllyImplInfo *impl)
{
	const ProllyHashRoutine *hash = (const ProllyHashRoutine *) impl;
	int			low[64] = {0};
	int			high[64] = {0};

	for (uint32 i = 0; i < 1024; i++)
	{
		ProllyHash	h;
		uint32		b;

		hash_once(hash, (const uint8 *) &i, sizeof(i), sizeof(i), &h);
		b = prolly_hash_bucket(h.bytes);
		low[b % 64]++;
		high[b >> 26]++;
	}
	for (int i = 0; i < 64; i++)
		if (low[i] == 0 || low[i] > 48 || high[i] == 0 || high[i] > 48)
			elog(ERROR, "digests' first four bytes don't spread evenly");
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

/*
 * A remote that lies, test only: "liar://path" is the file remote at path,
 * but it says it has every chunk, and keeps none it is sent.  Its heads
 * and what it already held are the file remote's.  For an archive's
 * check that it gives back what it was sent.
 */
static const ProllyRemoteRoutine *
liar_file(void)
{
	return prolly_remote_for_url("file:///");
}

static void
liar_check_url(const char *url)
{
	if (strncmp(url, "liar://", 7) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a liar remote's URL", url)));
}

static ProllyRemote *
liar_open(const char *url, bool create)
{
	liar_check_url(url);
	return liar_file()->open(psprintf("file://%s", url + 7), create);
}

static void
liar_close(ProllyRemote *r)
{
	liar_file()->close(r);
}

static void
liar_format(ProllyRemote *r, StringInfo out)
{
	liar_file()->format(r, out);
}

static void
liar_heads(ProllyRemote *r, ProllyRemoteHeadVisit visit, void *arg)
{
	liar_file()->heads(r, visit, arg);
}

static bool
liar_get(ProllyRemote *r, const ProllyHash *hash, StringInfo out)
{
	return liar_file()->get(r, hash, out);
}

static bool
liar_has(ProllyRemote *r, const ProllyHash *hash)
{
	return true;
}

static void
liar_has_many(ProllyRemote *r, const ProllyHash *hashes, int n, bool *has)
{
	for (int i = 0; i < n; i++)
		has[i] = true;
}

static void
liar_add_chunks(ProllyRemote *r, ProllyChunkNext next, void *arg)
{
	ProllyHash	h;
	StringInfoData data;

	initStringInfo(&data);
	while (next(arg, &h, &data))
		resetStringInfo(&data);
	pfree(data.data);
}

static bool
liar_move_head(ProllyRemote *r, const char *branch, const ProllyHash *expected,
			   const ProllyHash *head, const char *format, bool force)
{
	return liar_file()->move_head(r, branch, expected, head, format, force);
}

static void
liar_get_many(ProllyRemote *r, const ProllyHash *hashes, int n,
			  ProllyRemoteChunkVisit visit, void *arg)
{
	liar_file()->get_many(r, hashes, n, visit, arg);
}

static const ProllyRemoteRoutine test_liar_remote = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 9003,
		.name = "liar",
		.description = "test only: a file remote that says it has every chunk and keeps none",
	},
	.check_url = liar_check_url,
	.open = liar_open,
	.close = liar_close,
	.format = liar_format,
	.heads = liar_heads,
	.get = liar_get,
	.has = liar_has,
	.add_chunks = liar_add_chunks,
	.move_head = liar_move_head,
	.get_many = liar_get_many,
	.has_many = liar_has_many,
};

/*
 * A slow remote, test only: "slow://path" is the file remote at path, but
 * each chunk it takes takes four tenths of a second.  For a sever's
 * time limit, which is one without progress, not one in all.
 */
static ProllyRemote *
slow_open(const char *url, bool create)
{
	if (strncmp(url, "slow://", 7) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a slow remote's URL", url)));
	return liar_file()->open(psprintf("file://%s", url + 7), create);
}

static void
slow_check_url(const char *url)
{
	if (strncmp(url, "slow://", 7) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a slow remote's URL", url)));
}

typedef struct SlowNext
{
	ProllyChunkNext next;
	void	   *arg;
} SlowNext;

static bool
slow_next(void *arg, ProllyHash *hash, StringInfo data)
{
	SlowNext   *sn = arg;

	pg_usleep(400000);
	CHECK_FOR_INTERRUPTS();
	return sn->next(sn->arg, hash, data);
}

static void
slow_add_chunks(ProllyRemote *r, ProllyChunkNext next, void *arg)
{
	SlowNext	sn = {next, arg};

	liar_file()->add_chunks(r, slow_next, &sn);
}

static bool
slow_has(ProllyRemote *r, const ProllyHash *hash)
{
	return liar_file()->has(r, hash);
}

static void
slow_has_many(ProllyRemote *r, const ProllyHash *hashes, int n, bool *has)
{
	liar_file()->has_many(r, hashes, n, has);
}

static const ProllyRemoteRoutine test_slow_remote = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 9005,
		.name = "slow",
		.description = "test only: a file remote four tenths of a second a chunk",
	},
	.check_url = slow_check_url,
	.open = slow_open,
	.close = liar_close,
	.format = liar_format,
	.heads = liar_heads,
	.get = liar_get,
	.has = slow_has,
	.add_chunks = slow_add_chunks,
	.move_head = liar_move_head,
	.get_many = liar_get_many,
	.has_many = slow_has_many,
};

/*
 * A remote that never answers, test only: "stall://..." opens by waiting,
 * interruptibly, for ever.  For a sever's time limit on its archive.
 */
static void
stall_check_url(const char *url)
{
	if (strncmp(url, "stall://", 8) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a stall remote's URL", url)));
}

static ProllyRemote *
stall_open(const char *url, bool create)
{
	for (;;)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 100, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	return NULL;
}

static void
stall_close(ProllyRemote *r)
{
}

static const ProllyRemoteRoutine test_stall_remote = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 9004,
		.name = "stall",
		.description = "test only: a remote that never answers",
	},
	.check_url = stall_check_url,
	.open = stall_open,
	.close = stall_close,
};

void
_PG_init(void)
{
	prolly_register_impl(&test_fnv_hash.info);
	prolly_register_impl(&test_broken_hash.info);
	prolly_register_impl(&test_liar_remote.info);
	prolly_register_impl(&test_stall_remote.info);
	prolly_register_impl(&test_slow_remote.info);
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

		if (!prolly_interface_valid(iface))
			continue;
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

		if (!prolly_interface_valid(iface) ||
			(only_iface != NULL && iface != wanted))
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
