/*-------------------------------------------------------------------------
 *
 * prolly_registry.c
 *	  Registry of prolly interface implementations.
 *
 * Built-in implementations come from prolly_impllist.h.  Libraries in
 * shared_preload_libraries can add more with prolly_register_impl().
 *
 * The registry also knows each interface's callback names, so that "not
 * implemented" errors and the conformance suite can name callbacks.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_registry.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_registry.h"
#include "utils/memutils.h"

static const ProllyImplInfo *const builtin_impls[] = {
#define PG_PROLLY_IMPL(type, symbol) &symbol.info,
#include "access/prolly_impllist.h"
#undef PG_PROLLY_IMPL
};

/* Implementations added by prolly_register_impl(). */
static List *registered_impls = NIL;

/* Whether the built-in list has been checked for duplicates. */
static bool builtins_checked = false;

/*
 * What the registry knows about each interface.  A routine struct is a
 * ProllyImplInfo followed only by callback pointers, so the callbacks are
 * found by offset; the static assertions below keep the name lists in step
 * with the structs.
 */
typedef struct ProllyInterfaceDesc
{
	const char *name;
	Size		first_callback_offset;
	int			ncallbacks;
	const char *const *callback_names;
} ProllyInterfaceDesc;

static const char *const engine_callbacks[] = {PROLLY_ENGINE_CALLBACK_NAMES};
static const char *const coord_callbacks[] = {PROLLY_COORD_CALLBACK_NAMES};
static const char *const nodeformat_callbacks[] = {PROLLY_NODEFORMAT_CALLBACK_NAMES};
static const char *const chunker_callbacks[] = {PROLLY_CHUNKER_CALLBACK_NAMES};
static const char *const hash_callbacks[] = {PROLLY_HASH_CALLBACK_NAMES};
static const char *const keyenc_callbacks[] = {PROLLY_KEYENC_CALLBACK_NAMES};
static const char *const valenc_callbacks[] = {PROLLY_VALENC_CALLBACK_NAMES};
static const char *const nodestore_callbacks[] = {PROLLY_NODESTORE_CALLBACK_NAMES};
static const char *const alloc_callbacks[] = {PROLLY_ALLOC_CALLBACK_NAMES};
static const char *const reclaim_callbacks[] = {PROLLY_RECLAIM_CALLBACK_NAMES};
static const char *const log_callbacks[] = {PROLLY_LOG_CALLBACK_NAMES};
static const char *const commit_callbacks[] = {PROLLY_COMMIT_CALLBACK_NAMES};
static const char *const rowid_callbacks[] = {PROLLY_ROWID_CALLBACK_NAMES};
static const char *const refs_callbacks[] = {PROLLY_REFS_CALLBACK_NAMES};
static const char *const transfer_callbacks[] = {PROLLY_TRANSFER_CALLBACK_NAMES};
static const char *const merge_callbacks[] = {PROLLY_MERGE_CALLBACK_NAMES};
static const char *const tree_callbacks[] = {PROLLY_TREE_CALLBACK_NAMES};
static const char *const rootlayout_callbacks[] = {PROLLY_ROOTLAYOUT_CALLBACK_NAMES};
static const char *const keying_callbacks[] = {PROLLY_KEYING_CALLBACK_NAMES};
static const char *const ixkeying_callbacks[] = {PROLLY_IXKEYING_CALLBACK_NAMES};
static const char *const canon_callbacks[] = {PROLLY_CANON_CALLBACK_NAMES};
static const char *const chunkstore_callbacks[] = {PROLLY_CHUNKSTORE_CALLBACK_NAMES};

#define CHECK_ROUTINE(type, first, names) \
	static_assert(offsetof(type, info) == 0 && \
				  sizeof(type) == offsetof(type, first) + \
				  lengthof(names) * sizeof(pg_funcptr_t), \
				  "callback name list does not match " #type)

CHECK_ROUTINE(ProllyEngineRoutine, PROLLY_ENGINE_FIRST_CALLBACK, engine_callbacks);
CHECK_ROUTINE(ProllyCoordRoutine, PROLLY_COORD_FIRST_CALLBACK, coord_callbacks);
CHECK_ROUTINE(ProllyNodeFormatRoutine, PROLLY_NODEFORMAT_FIRST_CALLBACK, nodeformat_callbacks);
CHECK_ROUTINE(ProllyChunkerRoutine, PROLLY_CHUNKER_FIRST_CALLBACK, chunker_callbacks);
CHECK_ROUTINE(ProllyHashRoutine, PROLLY_HASH_FIRST_CALLBACK, hash_callbacks);
CHECK_ROUTINE(ProllyKeyEncoderRoutine, PROLLY_KEYENC_FIRST_CALLBACK, keyenc_callbacks);
CHECK_ROUTINE(ProllyValueEncoderRoutine, PROLLY_VALENC_FIRST_CALLBACK, valenc_callbacks);
CHECK_ROUTINE(ProllyNodeStoreRoutine, PROLLY_NODESTORE_FIRST_CALLBACK, nodestore_callbacks);
CHECK_ROUTINE(ProllyAllocatorRoutine, PROLLY_ALLOC_FIRST_CALLBACK, alloc_callbacks);
CHECK_ROUTINE(ProllyReclaimRoutine, PROLLY_RECLAIM_FIRST_CALLBACK, reclaim_callbacks);
CHECK_ROUTINE(ProllyLogRoutine, PROLLY_LOG_FIRST_CALLBACK, log_callbacks);
CHECK_ROUTINE(ProllyCommitRoutine, PROLLY_COMMIT_FIRST_CALLBACK, commit_callbacks);
CHECK_ROUTINE(ProllyRowIdRoutine, PROLLY_ROWID_FIRST_CALLBACK, rowid_callbacks);
CHECK_ROUTINE(ProllyRefsRoutine, PROLLY_REFS_FIRST_CALLBACK, refs_callbacks);
CHECK_ROUTINE(ProllyTransferRoutine, PROLLY_TRANSFER_FIRST_CALLBACK, transfer_callbacks);
CHECK_ROUTINE(ProllyMergeRoutine, PROLLY_MERGE_FIRST_CALLBACK, merge_callbacks);
CHECK_ROUTINE(ProllyTreeRoutine, PROLLY_TREE_FIRST_CALLBACK, tree_callbacks);
CHECK_ROUTINE(ProllyRootLayoutRoutine, PROLLY_ROOTLAYOUT_FIRST_CALLBACK, rootlayout_callbacks);
CHECK_ROUTINE(ProllyKeyingRoutine, PROLLY_KEYING_FIRST_CALLBACK, keying_callbacks);
CHECK_ROUTINE(ProllyIndexKeyingRoutine, PROLLY_IXKEYING_FIRST_CALLBACK, ixkeying_callbacks);
CHECK_ROUTINE(ProllyCanonRoutine, PROLLY_CANON_FIRST_CALLBACK, canon_callbacks);
CHECK_ROUTINE(ProllyChunkStoreRoutine, PROLLY_CHUNKSTORE_FIRST_CALLBACK, chunkstore_callbacks);

#define IFACE(name, type, first, names) \
	{name, offsetof(type, first), lengthof(names), names}

static const ProllyInterfaceDesc interfaces[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_ENGINE] =
	IFACE("engine", ProllyEngineRoutine, PROLLY_ENGINE_FIRST_CALLBACK, engine_callbacks),
	[PROLLY_IFACE_COORDINATION] =
	IFACE("coordination", ProllyCoordRoutine, PROLLY_COORD_FIRST_CALLBACK, coord_callbacks),
	[PROLLY_IFACE_NODE_FORMAT] =
	IFACE("node_format", ProllyNodeFormatRoutine, PROLLY_NODEFORMAT_FIRST_CALLBACK, nodeformat_callbacks),
	[PROLLY_IFACE_CHUNKER] =
	IFACE("chunker", ProllyChunkerRoutine, PROLLY_CHUNKER_FIRST_CALLBACK, chunker_callbacks),
	[PROLLY_IFACE_CONTENT_HASH] =
	IFACE("content_hash", ProllyHashRoutine, PROLLY_HASH_FIRST_CALLBACK, hash_callbacks),
	[PROLLY_IFACE_KEY_ENCODER] =
	IFACE("key_encoder", ProllyKeyEncoderRoutine, PROLLY_KEYENC_FIRST_CALLBACK, keyenc_callbacks),
	[PROLLY_IFACE_VALUE_ENCODER] =
	IFACE("value_encoder", ProllyValueEncoderRoutine, PROLLY_VALENC_FIRST_CALLBACK, valenc_callbacks),
	[PROLLY_IFACE_NODE_STORE] =
	IFACE("node_store", ProllyNodeStoreRoutine, PROLLY_NODESTORE_FIRST_CALLBACK, nodestore_callbacks),
	[PROLLY_IFACE_ALLOCATOR] =
	IFACE("allocator", ProllyAllocatorRoutine, PROLLY_ALLOC_FIRST_CALLBACK, alloc_callbacks),
	[PROLLY_IFACE_RECLAMATION] =
	IFACE("reclamation", ProllyReclaimRoutine, PROLLY_RECLAIM_FIRST_CALLBACK, reclaim_callbacks),
	[PROLLY_IFACE_LOG] =
	IFACE("log", ProllyLogRoutine, PROLLY_LOG_FIRST_CALLBACK, log_callbacks),
	[PROLLY_IFACE_COMMIT_PIPELINE] =
	IFACE("commit_pipeline", ProllyCommitRoutine, PROLLY_COMMIT_FIRST_CALLBACK, commit_callbacks),
	[PROLLY_IFACE_ROW_IDENTITY] =
	IFACE("row_identity", ProllyRowIdRoutine, PROLLY_ROWID_FIRST_CALLBACK, rowid_callbacks),
	[PROLLY_IFACE_REFS] =
	IFACE("refs", ProllyRefsRoutine, PROLLY_REFS_FIRST_CALLBACK, refs_callbacks),
	[PROLLY_IFACE_TRANSFER] =
	IFACE("transfer", ProllyTransferRoutine, PROLLY_TRANSFER_FIRST_CALLBACK, transfer_callbacks),
	[PROLLY_IFACE_DIFF_MERGE] =
	IFACE("diff_merge", ProllyMergeRoutine, PROLLY_MERGE_FIRST_CALLBACK, merge_callbacks),
	[PROLLY_IFACE_TREE] =
	IFACE("tree", ProllyTreeRoutine, PROLLY_TREE_FIRST_CALLBACK, tree_callbacks),
	[PROLLY_IFACE_ROOT_LAYOUT] =
	IFACE("root_layout", ProllyRootLayoutRoutine, PROLLY_ROOTLAYOUT_FIRST_CALLBACK, rootlayout_callbacks),
	[PROLLY_IFACE_TABLE_KEYING] =
	IFACE("table_keying", ProllyKeyingRoutine, PROLLY_KEYING_FIRST_CALLBACK, keying_callbacks),
	[PROLLY_IFACE_INDEX_KEYING] =
	IFACE("index_keying", ProllyIndexKeyingRoutine, PROLLY_IXKEYING_FIRST_CALLBACK, ixkeying_callbacks),
	[PROLLY_IFACE_CANONICALIZER] =
	IFACE("canonicalizer", ProllyCanonRoutine, PROLLY_CANON_FIRST_CALLBACK, canon_callbacks),
	[PROLLY_IFACE_CHUNK_STORE] =
	IFACE("chunk_store", ProllyChunkStoreRoutine, PROLLY_CHUNKSTORE_FIRST_CALLBACK, chunkstore_callbacks),
};

static const ProllyInterfaceDesc *
get_interface(ProllyInterface iface)
{
	if (iface < PROLLY_IFACE_FIRST || iface > PROLLY_IFACE_LAST)
		elog(ERROR, "invalid prolly interface %d", (int) iface);
	return &interfaces[iface];
}

/*
 * Report a second implementation with the same interface and name or
 * impl_id as an existing one.
 */
static void
check_conflict(const ProllyImplInfo *existing, const ProllyImplInfo *impl)
{
	if (existing->iface != impl->iface)
		return;
	if (strcmp(existing->name, impl->name) == 0)
		elog(ERROR, "duplicate %s implementation name \"%s\"",
			 prolly_interface_name(impl->iface), impl->name);
	if (existing->impl_id == impl->impl_id)
		elog(ERROR, "%s implementations \"%s\" and \"%s\" have the same impl_id %u",
			 prolly_interface_name(impl->iface), existing->name, impl->name,
			 (unsigned) impl->impl_id);
}

static void
check_builtins(void)
{
	if (builtins_checked)
		return;

	for (int i = 0; i < lengthof(builtin_impls); i++)
	{
		(void) get_interface(builtin_impls[i]->iface);
		for (int j = 0; j < i; j++)
			check_conflict(builtin_impls[j], builtin_impls[i]);
	}
	builtins_checked = true;
}

/*
 * Calls fn on every implementation of iface; stops early if fn returns
 * true, and returns that implementation.
 */
static const ProllyImplInfo *
find_impl(ProllyInterface iface,
		  bool (*fn) (const ProllyImplInfo *impl, const void *arg),
		  const void *arg)
{
	ListCell   *lc;

	check_builtins();

	for (int i = 0; i < lengthof(builtin_impls); i++)
	{
		if (builtin_impls[i]->iface == iface && fn(builtin_impls[i], arg))
			return builtin_impls[i];
	}
	foreach(lc, registered_impls)
	{
		const ProllyImplInfo *impl = lfirst(lc);

		if (impl->iface == iface && fn(impl, arg))
			return impl;
	}
	return NULL;
}

static bool
match_name(const ProllyImplInfo *impl, const void *arg)
{
	return strcmp(impl->name, (const char *) arg) == 0;
}

static bool
match_id(const ProllyImplInfo *impl, const void *arg)
{
	return impl->impl_id == *(const uint16 *) arg;
}

static bool
match_all(const ProllyImplInfo *impl, const void *arg)
{
	List	  **result = (List **) arg;

	*result = lappend(*result, unconstify(ProllyImplInfo *, impl));
	return false;
}

const ProllyImplInfo *
prolly_lookup_impl(ProllyInterface iface, const char *name)
{
	(void) get_interface(iface);
	return find_impl(iface, match_name, name);
}

const ProllyImplInfo *
prolly_lookup_impl_id(ProllyInterface iface, uint16 impl_id)
{
	(void) get_interface(iface);
	return find_impl(iface, match_id, &impl_id);
}

const ProllyImplInfo *
prolly_get_impl(ProllyInterface iface, const char *name)
{
	const ProllyImplInfo *impl = prolly_lookup_impl(iface, name);

	if (impl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%s implementation \"%s\" does not exist",
						prolly_interface_name(iface), name)));
	return impl;
}

List *
prolly_list_impls(ProllyInterface iface)
{
	List	   *result = NIL;

	(void) get_interface(iface);
	(void) find_impl(iface, match_all, &result);
	return result;
}

void
prolly_register_impl(const ProllyImplInfo *impl)
{
	MemoryContext oldcxt;
	ListCell   *lc;

	(void) get_interface(impl->iface);
	if (impl->name == NULL || impl->name[0] == '\0')
		elog(ERROR, "prolly implementation must have a name");

	check_builtins();
	for (int i = 0; i < lengthof(builtin_impls); i++)
		check_conflict(builtin_impls[i], impl);
	foreach(lc, registered_impls)
		check_conflict(lfirst(lc), impl);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	registered_impls = lappend(registered_impls,
							   unconstify(ProllyImplInfo *, impl));
	MemoryContextSwitchTo(oldcxt);
}

const char *
prolly_interface_name(ProllyInterface iface)
{
	return get_interface(iface)->name;
}

bool
prolly_interface_from_name(const char *name, ProllyInterface *iface)
{
	for (int i = PROLLY_IFACE_FIRST; i <= PROLLY_IFACE_LAST; i++)
	{
		if (strcmp(interfaces[i].name, name) == 0)
		{
			*iface = (ProllyInterface) i;
			return true;
		}
	}
	return false;
}

int
prolly_interface_ncallbacks(ProllyInterface iface)
{
	return get_interface(iface)->ncallbacks;
}

const char *
prolly_interface_callback_name(ProllyInterface iface, int callbackno)
{
	const ProllyInterfaceDesc *desc = get_interface(iface);

	if (callbackno < 0 || callbackno >= desc->ncallbacks)
		elog(ERROR, "invalid callback number %d for prolly interface %s",
			 callbackno, desc->name);
	return desc->callback_names[callbackno];
}

bool
prolly_impl_has_callback(const ProllyImplInfo *impl, int callbackno)
{
	const ProllyInterfaceDesc *desc = get_interface(impl->iface);
	pg_funcptr_t fn;

	if (callbackno < 0 || callbackno >= desc->ncallbacks)
		elog(ERROR, "invalid callback number %d for prolly interface %s",
			 callbackno, desc->name);

	/* The info is the first member, so impl is the routine's address. */
	memcpy(&fn,
		   (const char *) impl + desc->first_callback_offset +
		   callbackno * sizeof(pg_funcptr_t),
		   sizeof(fn));
	return fn != NULL;
}

void
prolly_not_implemented(const ProllyImplInfo *impl, const char *callback)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s implementation \"%s\" does not implement %s",
					prolly_interface_name(impl->iface), impl->name,
					callback)));
	pg_unreachable();
}
