/*-------------------------------------------------------------------------
 *
 * prolly_impl.h
 *	  The interface -> implementation pattern used by all prolly components.
 *
 * Every prolly component is an interface: a routine struct of callbacks,
 * declared in its own header (prolly_tree.h, prolly_hash.h, ...).  Every
 * routine struct starts with a ProllyImplInfo, followed only by callback
 * pointers.  Implementations fill in a const routine struct and are found by
 * name through the registry (prolly_registry.h).
 *
 * A callback left NULL means "not implemented": calling it through
 * PROLLY_CALL raises ERRCODE_FEATURE_NOT_SUPPORTED naming the interface, the
 * implementation and the callback.  A stub implementation is therefore a
 * routine with every callback NULL, and a real implementation can be built
 * up one callback at a time.  The conformance suite
 * (src/test/modules/test_prolly) reports which callbacks are missing.
 *
 * src/include/access/prolly_impl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_IMPL_H
#define PROLLY_IMPL_H

/*
 * The interfaces.  Never renumber or reuse one; add new interfaces at the
 * end.  1-3, 8, 9, 11-13, 18 and 20 were the tree engines' (engine,
 * coordination, node format, node store, allocator, log, commit pipeline,
 * row identity, root layout, index keying), removed with them: the
 * registry has no interface by those numbers.
 */
typedef enum ProllyInterface
{
	PROLLY_IFACE_CHUNKER = 4,
	PROLLY_IFACE_CONTENT_HASH = 5,
	PROLLY_IFACE_KEY_ENCODER = 6,
	PROLLY_IFACE_VALUE_ENCODER = 7,
	PROLLY_IFACE_RECLAMATION = 10,
	PROLLY_IFACE_REFS = 14,
	PROLLY_IFACE_TRANSFER = 15,
	PROLLY_IFACE_DIFF_MERGE = 16,
	PROLLY_IFACE_TREE = 17,
	PROLLY_IFACE_TABLE_KEYING = 19,
	PROLLY_IFACE_CANONICALIZER = 21,
	PROLLY_IFACE_CHUNK_STORE = 22,
	PROLLY_IFACE_CHUNK_FORMAT = 23,
	PROLLY_IFACE_ROW_APPLIER = 24,
	PROLLY_IFACE_TABLE_FILE = 25,
	PROLLY_IFACE_CHANGE_FINDER = 26,
	PROLLY_IFACE_REMOTE = 27,
	PROLLY_IFACE_VALUE_MEMO = 28,
	PROLLY_IFACE_WRITERS = 29,
	PROLLY_IFACE_CHANGE_LOG = 30,
	PROLLY_IFACE_MOVE_PLANNER = 31,
	PROLLY_IFACE_MATERIALIZATION_CACHE = 32,
	PROLLY_IFACE_JSON_TREE = 33,
	PROLLY_IFACE_NODE_CACHE = 34,
	PROLLY_IFACE_COMMIT_CACHE = 35,
	PROLLY_IFACE_HISTORY_WINDOW = 36,
	PROLLY_IFACE_ROOT_FREEZER = 38,
} ProllyInterface;

#define PROLLY_IFACE_FIRST	PROLLY_IFACE_CHUNKER
#define PROLLY_IFACE_LAST	PROLLY_IFACE_ROOT_FREEZER

/*
 * Identity of an implementation.  The first member of every routine struct.
 *
 * impl_id is unique within its interface and is what gets persisted; name is
 * unique within its interface and is what configuration uses.  impl_id 0 is
 * reserved for the stub, which never persists anything.  format_version is
 * bumped whenever anything the implementation persists changes format.
 */
typedef struct ProllyImplInfo
{
	ProllyInterface iface;
	uint16		impl_id;
	uint16		format_version;
	const char *name;
	const char *description;
} ProllyImplInfo;

#define PROLLY_STUB_IMPL_ID		0

/* Raises ERRCODE_FEATURE_NOT_SUPPORTED for a missing callback. */
pg_noreturn extern void prolly_not_implemented(const ProllyImplInfo *impl,
											   const char *callback);

/*
 * Call a callback through a routine struct:
 *
 *		PROLLY_CALL(hash, update, ctx, data, len)
 *
 * evaluates hash->update(ctx, data, len), or raises "not implemented" if the
 * implementation leaves update NULL.  "routine" is evaluated more than once,
 * so pass a plain variable.
 */
#define PROLLY_CALL(routine, callback, ...) \
	(((routine)->callback != NULL ? (routine)->callback : \
	  (prolly_not_implemented(&(routine)->info, #callback), \
	   (routine)->callback)) (__VA_ARGS__))

/* True if the implementation provides the callback. */
#define PROLLY_HAS(routine, callback) ((routine)->callback != NULL)

#endif							/* PROLLY_IMPL_H */
