/*-------------------------------------------------------------------------
 *
 * prolly_impl.h
 *	  The interface -> implementation pattern used by all prolly components.
 *
 * Every prolly component is an interface: a routine struct of callbacks,
 * declared in its own header (prolly_engine.h, prolly_hash.h, ...).  Every
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
 * See specs/00-architecture-principles.md for the rules.
 *
 * src/include/access/prolly_impl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_IMPL_H
#define PROLLY_IMPL_H

/*
 * The interfaces.  These numbers are persisted in format stamps (rule 5), so
 * never renumber or reuse one; add new interfaces at the end.
 */
typedef enum ProllyInterface
{
	PROLLY_IFACE_ENGINE = 1,
	PROLLY_IFACE_COORDINATION = 2,
	PROLLY_IFACE_NODE_FORMAT = 3,
	PROLLY_IFACE_CHUNKER = 4,
	PROLLY_IFACE_CONTENT_HASH = 5,
	PROLLY_IFACE_KEY_ENCODER = 6,
	PROLLY_IFACE_VALUE_ENCODER = 7,
	PROLLY_IFACE_NODE_STORE = 8,
	PROLLY_IFACE_ALLOCATOR = 9,
	PROLLY_IFACE_RECLAMATION = 10,
	PROLLY_IFACE_LOG = 11,
	PROLLY_IFACE_COMMIT_PIPELINE = 12,
	PROLLY_IFACE_ROW_IDENTITY = 13,
	PROLLY_IFACE_REFS = 14,
	PROLLY_IFACE_TRANSFER = 15,
	PROLLY_IFACE_DIFF_MERGE = 16,
} ProllyInterface;

#define PROLLY_IFACE_FIRST	PROLLY_IFACE_ENGINE
#define PROLLY_IFACE_LAST	PROLLY_IFACE_DIFF_MERGE

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

/*
 * What persisted data records about the implementation that wrote it
 * (rule 5).  Serialize field by field; don't write the struct's bytes.
 */
typedef struct ProllyFormatStamp
{
	uint8		iface;			/* ProllyInterface */
	uint16		impl_id;
	uint16		format_version;
} ProllyFormatStamp;

static inline ProllyFormatStamp
prolly_format_stamp(const ProllyImplInfo *impl)
{
	ProllyFormatStamp stamp;

	stamp.iface = (uint8) impl->iface;
	stamp.impl_id = impl->impl_id;
	stamp.format_version = impl->format_version;
	return stamp;
}

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
