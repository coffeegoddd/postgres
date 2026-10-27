/*-------------------------------------------------------------------------
 *
 * toast_chunkstore.c
 *	  The TOAST destination of BLAKE3 hash pointers: value trees in the
 *	  current database's chunk store; see prolly_toast.h.
 *
 * A value is stored as the tree implementation's value tree, in the
 * environment history's trees use (prolly_folded_tree_env()), so its root
 * is the reference a fold makes of the column (the value encoder's):
 * the tree is built over the value's canonical bytes.  For bytea, and
 * text, varchar and bpchar in a UTF-8 database, those are the bytes the
 * value holds; a text-like value in another encoding is converted to UTF-8
 * first, and a jsonb document encoded (a version byte, its strings in
 * UTF-8).  Fetching converts back.  A slice of a value stored as its bytes
 * reads only the tree's nodes that hold it; one converted is read whole.
 *
 * A jsonb document whose canonical bytes are too long to be inline in a
 * row is stored as a JSON tree instead (prolly_jsontree.h), built from
 * those bytes: a tree keyed by path, which is the column's tree in history
 * too, so a fold takes it unread, and a path is read or a value set
 * (prolly_toast_json_tree()) without the whole document.  Fetching it
 * reads every item, and converts the document to the database's encoding.
 * A document that can't be a tree (a scalar, or a path too long for a
 * key) is stored as its bytes.
 *
 * A value is stored converted only if fetching gives its bytes back: the
 * conversion is checked to undo itself (it doesn't for characters of
 * another encoding that map to one Unicode character), and the converted
 * value is small enough to convert back.  Otherwise, and for every other
 * type, the tree holds the value's bytes, which are not its canonical
 * bytes: a fold reads such a value rather than take its tree.
 *
 * The toaster stores the values it moves out of a versioned table's rows
 * here (save()), the table's access method choosing this destination, and
 * keeps a hash pointer a row is given as it is (reference()).  History's
 * value trees are named by pointers made from their references, unread
 * (prolly_toast_pointer()), so a row written from history (a reset, a
 * merge) holds the tree history has.
 *
 * A tree is the chunk store's, not a row's: nothing deletes it with the
 * rows that name it.  A collection of the store keeps every tree a hash
 * pointer on a page names (prolly_toast_roots()), and each tree stored is
 * told to the store's keep(), for a collection running meanwhile.  The
 * pages are read from the page stores, only the ranges of blocks written
 * since the last collection: what it found in the rest is in a file of the
 * chunk store's directory a page store, which the page store's token for
 * that scan says still holds.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/toast_chunkstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/detoast.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/prolly_canon.h"
#include "access/prolly_datum.h"
#include "access/prolly_toast.h"
#include "access/prolly_tree.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/fd.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "varatt.h"

/* the trees' environment, the current database's */
static ProllyTreeEnv env;
static const ProllyTreeRoutine *tree;
static bool env_ready = false;
static ProllyNodeCounters counters; /* this backend's reads and writes */
static ProllyDatumCodec *jsonb_codec;
static const ProllyJsonTreeRoutine *json_tree;


static void
ensure_env(void)
{
	if (env_ready)
		return;
	if (!OidIsValid(MyDatabaseId))
		elog(ERROR, "values by content need a database");
	tree = prolly_tree_lookup("cow");
	json_tree = prolly_jsontree_lookup("paths");
	prolly_folded_tree_env(&env);
	env.counters = &counters;
	env_ready = true;
}

static ProllyDatumCodec *
jsonb(void)
{
	if (jsonb_codec == NULL)
		jsonb_codec = prolly_datum_codec(JSONBOID, -1, TopMemoryContext);
	return jsonb_codec;
}

/*
 * The form whose tree holds the canonical bytes (prolly_datum.h) of a value
 * of type typid, -1 if none does: bytea's bytes; text-like values' bytes in
 * UTF-8, or in UTF-8 converted from another encoding (SQL_ASCII's bytes
 * are no encoding's); a jsonb document's canonical bytes.
 */
static int
canonical_form(Oid typid)
{
	Oid			base = getBaseType(typid);

	switch (base)
	{
		case BYTEAOID:
			return PROLLY_TOAST_FORM_BYTES;
		case JSONBOID:
			return PROLLY_TOAST_FORM_JSONB;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			if (GetDatabaseEncoding() == PG_UTF8)
				return PROLLY_TOAST_FORM_BYTES;
			if (GetDatabaseEncoding() != PG_SQL_ASCII)
				return PROLLY_TOAST_FORM_UTF8;
			return -1;
	}
	return -1;
}

/*
 * The form a value of type typid is stored in, if it can be: its canonical
 * form, else its bytes.
 */
static int
toast_form(Oid typid)
{
	int			form = canonical_form(typid);

	return form < 0 ? PROLLY_TOAST_FORM_BYTES : form;
}

/* A hash pointer: to a tree (root, len bytes) of a form, of a raw size */
static varlena *
make_pointer(const ProllyHash *root, uint64 len, int form, Size rawsize)
{
	varatt_external_hash p;
	varlena    *result;

	Assert(root->len == sizeof(p.va_hash));
	p.va_rawsize = rawsize;
	p.va_extinfo = (uint32) len | ((uint32) form << VARLENA_EXTSIZE_BITS);
	memcpy(p.va_hash, root->bytes, sizeof(p.va_hash));
	result = (varlena *) palloc(VARHDRSZ_EXTERNAL + sizeof(p));
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK_BLAKE3);
	memcpy(VARDATA_EXTERNAL(result), &p, sizeof(p));
	return result;
}

/* A hash pointer's form, and its tree's root and length (*root, *len) */
static int
pointer_root(const varlena *pointer, ProllyHash *root, uint64 *len)
{
	varatt_external_hash p;

	VARATT_EXTERNAL_GET_POINTER(p, pointer);
	root->len = sizeof(p.va_hash);
	memcpy(root->bytes, p.va_hash, sizeof(p.va_hash));
	*len = VARATT_EXTINFO_GET_EXTSIZE(p.va_extinfo);
	return p.va_extinfo >> VARLENA_EXTSIZE_BITS;
}

/* The longest string an encoding conversion takes */
#define CONVERTIBLE_MAX		(MaxAllocSize / MAX_CONVERSION_GROWTH - 1)

/* The longest value fetched whole: its bytes and header in a StringInfo */
#define FETCHABLE_MAX		(MaxAllocSize - 1 - VARHDRSZ)

/*
 * data, len bytes of the server's encoding, in UTF-8 into out, if
 * converting them back gives them back; false if not, or if either is too
 * long to convert.
 */
static bool
to_utf8(const char *data, Size len, StringInfo out)
{
	char	   *u;
	char	   *back;
	Size		ulen;
	bool		same;

	if (len > CONVERTIBLE_MAX)
		return false;
	u = pg_server_to_any(data, len, PG_UTF8);
	ulen = (u == data) ? len : strlen(u);
	if (ulen > CONVERTIBLE_MAX)
		return false;
	back = (u == data) ? u : pg_any_to_server(u, ulen, PG_UTF8);
	same = (back == u) ? (ulen == len) :
		(strlen(back) == len && memcmp(back, data, len) == 0);
	if (same)
		appendBinaryStringInfo(out, u, ulen);
	if (back != u && back != data)
		pfree(back);
	if (u != data)
		pfree(u);
	return same;
}

/*
 * A jsonb document's canonical bytes into out, if decoding them gives its
 * bytes back (they hold its strings in UTF-8, which in another encoding
 * may not convert back, or sort its keys otherwise).
 */
static bool
to_canonical_jsonb(varlena *plain, StringInfo out)
{
	varlena    *back;
	bool		same;

	prolly_datum_encode(jsonb(), PointerGetDatum(plain), out);
	if (GetDatabaseEncoding() == PG_UTF8)
		return true;
	back = (varlena *) DatumGetPointer(prolly_datum_decode(jsonb(), out->data,
														   out->len));
	same = VARSIZE(back) == VARSIZE_ANY(plain) &&
		memcmp(VARDATA(back), VARDATA_ANY(plain), VARSIZE_ANY_EXHDR(plain)) == 0;
	pfree(back);
	if (!same)
		resetStringInfo(out);
	return same;
}

varlena *
prolly_toast_store(Datum value, Oid typid)
{
	varlena    *v = (varlena *) DatumGetPointer(value);
	varlena    *plain = v;
	const char *data;
	Size		len;
	StringInfoData canonical;
	StringInfoData ref;
	ProllyValue r;
	ProllyHash	root;
	uint64		treelen;
	int			form = PROLLY_TOAST_FORM_BYTES;
	varatt_external_hash p;
	varlena    *result;

	ensure_env();
	if (VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v))
		plain = detoast_attr(v);
	data = VARDATA_ANY(plain);
	len = VARSIZE_ANY_EXHDR(plain);

	initStringInfo(&canonical);
	switch (toast_form(typid))
	{
		case PROLLY_TOAST_FORM_JSONB:
			if (to_canonical_jsonb(plain, &canonical))
				form = PROLLY_TOAST_FORM_JSONB;
			break;
		case PROLLY_TOAST_FORM_UTF8:
			if (to_utf8(data, len, &canonical))
				form = PROLLY_TOAST_FORM_UTF8;
			break;
	}

	/* a document's canonical bytes are a version byte and its binary */
	if (form == PROLLY_TOAST_FORM_JSONB && canonical.len > PROLLY_JSON_TREE_MIN)
	{
		ProllyNodeRef jroot;

		if (PROLLY_CALL(json_tree, build, &env, canonical.data + 1,
						canonical.len - 1, &jroot))
		{
			result = prolly_toast_json_pointer(&jroot, len + VARHDRSZ);
			pfree(canonical.data);
			if (plain != v)
				pfree(plain);
			return result;
		}
	}
	if (form != PROLLY_TOAST_FORM_BYTES)
	{
		data = canonical.data;
		len = canonical.len;
	}
	if (len > FETCHABLE_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("value of %zu bytes is too large to store by content", len)));

	initStringInfo(&ref);
	PROLLY_CALL(tree, value_write, &env, (const uint8 *) data, len, &ref);
	r.data = (const uint8 *) ref.data;
	r.len = ref.len;
	PROLLY_CALL(tree, value_root, &r, &root, &treelen);
	if (root.len != sizeof(p.va_hash) || treelen != len)
		elog(ERROR, "a value tree's root isn't a BLAKE3 hash pointer's");
	PROLLY_CALL(env.chunk_store, keep, env.chunks, &root);

	result = make_pointer(&root, len, form, VARSIZE_ANY_EXHDR(plain) + VARHDRSZ);

	pfree(ref.data);
	pfree(canonical.data);
	if (plain != v)
		pfree(plain);
	return result;
}

bool
prolly_toast_canonical(const varlena *pointer, Oid typid,
					   const ProllyValueStore *values, StringInfo ref,
					   uint64 *len)
{
	ProllyHash	root;
	int			form;

	Assert(VARATT_IS_EXTERNAL_ONDISK_HASH(pointer));
	ensure_env();
	form = pointer_root(pointer, &root, len);

	/*
	 * A long document is the column's tree in history as a JSON tree; one
	 * stored as its bytes is read, to be one if it can.
	 */
	if (form == PROLLY_TOAST_FORM_JSON_TREE)
	{
		if (canonical_form(typid) != PROLLY_TOAST_FORM_JSONB)
			return false;
	}
	else if (form != canonical_form(typid) || form == PROLLY_TOAST_FORM_JSONB)
		return false;

	/* the trees of history's value store are these, in this store */
	if (values->tree != tree ||
		prolly_local_chunks_store(values->env->chunk_store) !=
		prolly_local_chunks_store(env.chunk_store) ||
		values->env->chunks != env.chunks)
		return false;

	/* (a tree that isn't there is an error when the value is read) */
	if (!PROLLY_CALL(env.chunk_store, has, env.chunks, &root))
		return false;
	(void) prolly_toast_reference(pointer, ref);
	return true;
}

varlena *
prolly_toast_pointer(const ProllyHash *root, uint64 len, Oid typid)
{
	/*
	 * Only a value stored as its bytes: one converted (text in another
	 * encoding, jsonb) has its size known only once converted back, and a
	 * jsonb document from history is checked as it is decoded.
	 */
	if (canonical_form(typid) != PROLLY_TOAST_FORM_BYTES ||
		root->len != sizeof(((varatt_external_hash *) NULL)->va_hash) ||
		len > FETCHABLE_MAX)
		return NULL;
	return make_pointer(root, len, PROLLY_TOAST_FORM_BYTES, len + VARHDRSZ);
}

int
prolly_toast_reference(const varlena *pointer, StringInfo ref)
{
	ProllyHash	root;
	uint64		len;
	int			form;

	Assert(VARATT_IS_EXTERNAL_ONDISK_HASH(pointer));
	ensure_env();
	form = pointer_root(pointer, &root, &len);
	/* (a JSON tree's reference says it is one by its length, 0) */
	PROLLY_CALL(tree, value_reference, &root,
				form == PROLLY_TOAST_FORM_JSON_TREE ? 0 : len, ref);
	return form;
}

bool
prolly_toast_json_tree(const varlena *pointer, ProllyNodeRef *root,
					   Size *rawsize)
{
	varatt_external_hash p;
	uint64		len;

	if (!VARATT_IS_EXTERNAL_ONDISK_HASH(pointer))
		return false;
	ensure_env();
	memset(root, 0, sizeof(*root));
	if (pointer_root(pointer, &root->hash, &len) != PROLLY_TOAST_FORM_JSON_TREE)
		return false;
	VARATT_EXTERNAL_GET_POINTER(p, pointer);
	*rawsize = p.va_rawsize;
	return true;
}

varlena *
prolly_toast_json_pointer(const ProllyNodeRef *root, Size rawsize)
{
	ensure_env();
	PROLLY_CALL(env.chunk_store, keep, env.chunks, &root->hash);
	/* (a tree's document is longer than any inline one, whatever is known) */
	rawsize = Max(rawsize, VARHDRSZ + PROLLY_JSON_TREE_MIN + 1);
	rawsize = Min(rawsize, (Size) VARLENA_EXTSIZE_MASK);
	return make_pointer(&root->hash, rawsize - VARHDRSZ,
						PROLLY_TOAST_FORM_JSON_TREE, rawsize);
}

const ProllyTreeEnv *
prolly_toast_env(void)
{
	ensure_env();
	return &env;
}

bool
prolly_toast_readable(const varlena *pointer)
{
	varatt_external_hash p;
	ProllyHash	root;

	Assert(VARATT_IS_EXTERNAL_ONDISK_HASH(pointer));
	ensure_env();
	VARATT_EXTERNAL_GET_POINTER(p, pointer);
	root.len = sizeof(p.va_hash);
	memcpy(root.bytes, p.va_hash, sizeof(p.va_hash));
	return PROLLY_CALL(env.chunk_store, has, env.chunks, &root);
}

void
prolly_toast_counters(ProllyNodeCounters *out, bool reset)
{
	*out = counters;
	if (reset)
		memset(&counters, 0, sizeof(counters));
}

/* a varlena of n bytes, its header made, its data to fill */
static varlena *
new_varlena(Size n)
{
	varlena    *v = palloc(VARHDRSZ + n);

	SET_VARSIZE(v, VARHDRSZ + n);
	return v;
}

/* Bytes [offset, offset + length) of v (length < 0: to its end), copied. */
static varlena *
slice(varlena *v, int32 offset, int32 length)
{
	Size		size = VARSIZE(v) - VARHDRSZ;
	Size		n;
	varlena    *result;

	if ((Size) offset >= size)
		n = 0;
	else if (length < 0 || (Size) length > size - offset)
		n = size - offset;
	else
		n = length;
	result = new_varlena(n);
	memcpy(VARDATA(result), VARDATA(v) + Min((Size) offset, size), n);
	return result;
}

static varlena *
chunkstore_fetch(varlena *attr, int32 offset, int32 length)
{
	StringInfoData ref;
	StringInfoData out;
	ProllyValue r;
	int			form;
	varlena    *whole;
	varlena    *result;

	INJECTION_POINT("prolly-toast-fetch", NULL);
	initStringInfo(&ref);
	form = prolly_toast_reference(attr, &ref);
	if (form == PROLLY_TOAST_FORM_JSON_TREE)
	{
		ProllyNodeRef jroot;
		uint64		len;

		(void) pointer_root(attr, &jroot.hash, &len);
		whole = (varlena *) PROLLY_CALL(json_tree, read, &env, &jroot);
		pfree(ref.data);
		if (offset == 0 && length < 0)
			return whole;
		result = slice(whole, offset, length);
		pfree(whole);
		return result;
	}
	r.data = (const uint8 *) ref.data;
	r.len = ref.len;
	initStringInfo(&out);
	appendBinaryStringInfo(&out, "\0\0\0\0", VARHDRSZ);	/* the header's room */

	switch (form)
	{
		case PROLLY_TOAST_FORM_BYTES:
			if (length != 0)
				PROLLY_CALL(tree, value_read, &env, &r, offset,
							length < 0 ? PG_UINT64_MAX : (uint64) length, &out);
			SET_VARSIZE(out.data, out.len);
			pfree(ref.data);
			return (varlena *) out.data;
		case PROLLY_TOAST_FORM_UTF8:
			{
				char	   *s;
				Size		n;

				PROLLY_CALL(tree, value_read, &env, &r, 0, PG_UINT64_MAX, &out);
				s = pg_any_to_server(out.data + VARHDRSZ, out.len - VARHDRSZ,
									 PG_UTF8);
				n = (s == out.data + VARHDRSZ) ? out.len - VARHDRSZ : strlen(s);
				whole = new_varlena(n);
				memcpy(VARDATA(whole), s, n);
				if (s != out.data + VARHDRSZ)
					pfree(s);
				break;
			}
		case PROLLY_TOAST_FORM_JSONB:
			PROLLY_CALL(tree, value_read, &env, &r, 0, PG_UINT64_MAX, &out);
			whole = (varlena *)
				DatumGetPointer(prolly_datum_decode(jsonb(), out.data + VARHDRSZ,
													out.len - VARHDRSZ));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("hash pointer of unknown form %d", form)));
			whole = NULL;		/* keep compiler quiet */
	}
	pfree(ref.data);
	pfree(out.data);
	if (offset == 0 && length < 0)
		return whole;
	result = slice(whole, offset, length);
	pfree(whole);
	return result;
}

/*
 * What a scan of a page store found, by range of its blocks
 * (bmap_scan_store()), so the next collection reads again only the ranges
 * written since: a file a store in the chunk store's directory.  A header
 * (ROOTS_MAGIC, ROOTS_VERSION, the scan's token), then a record a range
 * that holds any hash pointer, in order of range: the range, its count of
 * hashes, and the hashes, each once, each with the generation of the
 * chunk store's settled file holding it (0: none did, as it was found).
 * The page store's token says whether it still holds.  It is not synced:
 * a crash makes every token stale, and a file that doesn't hold together
 * is discarded, every block read again.
 *
 * Beside it, a file of the same hashes by generation (EPOCHS_SUFFIX), so a
 * retire of some generations reads only theirs: a header (EPOCHS_MAGIC,
 * the version, the same token), the count of generations, then for each,
 * ascending, the generation and its count of hashes; then the hashes, in
 * that order.
 */
#define ROOTS_PREFIX		"prolly_page_roots_"	/* and the tablespace */
#define ROOTS_MAGIC			0x50524F54	/* "PROT" */
#define ROOTS_VERSION		2
#define ROOTS_HASH_LEN		sizeof(((varatt_external_hash *) NULL)->va_hash)
#define ROOTS_ENTRY			(ROOTS_HASH_LEN + sizeof(uint32))
#define EPOCHS_SUFFIX		".epochs"
#define EPOCHS_MAGIC		0x50524F45	/* "PROE" */

typedef struct RootsHeader
{
	uint32		magic;
	uint32		version;
	uint64		token;
} RootsHeader;

typedef struct RootsRange
{
	uint32		range;
	uint32		n;
} RootsRange;

/* the bytes a hash pointer starts with, and what a store's scan keeps */
typedef struct RootScan
{
	uint8		head[VARHDRSZ_EXTERNAL];
	ProllyChunkRefVisit visit;
	void	   *arg;

	/* the last scan's file, read up to a range's record, old_at */
	FILE	   *old;
	const char *old_path;
	RootsRange	old_at;			/* (range PG_UINT32_MAX: none left) */

	/* this scan's file, and the range being read */
	FILE	   *out;
	const char *out_path;
	uint32		range;
	bool		reading;
	uint8	   *found;			/* its hashes, ROOTS_ENTRY bytes each */
	uint32		nfound;
	uint32		maxfound;

	/* every hash written, ROOTS_ENTRY bytes each: the file by generation */
	uint8	   *all;
	uint64		nall;
	uint64		maxall;

	/* a retire's (retire_lo 0: not one): the ranges unchanged not read */
	uint32		retire_lo;
	uint32		retire_hi;
} RootScan;

static void
roots_read(RootScan *rs, void *buf, size_t len)
{
	if (fread(buf, 1, len, rs->old) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", rs->old_path)));
}

static void
roots_write(RootScan *rs, const void *buf, size_t len)
{
	if (fwrite(buf, 1, len, rs->out) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", rs->out_path)));
}

/* The last scan's next record: its range and count read. */
static void
roots_next(RootScan *rs)
{
	uint32		last = rs->old_at.range;
	size_t		got = fread(&rs->old_at, 1, sizeof(rs->old_at), rs->old);

	if (got == 0 && feof(rs->old))
	{
		rs->old_at.range = PG_UINT32_MAX;
		rs->old_at.n = 0;
		return;
	}
	if (got != sizeof(rs->old_at) ||
		(last != PG_UINT32_MAX && rs->old_at.range <= last))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" is corrupt", rs->old_path)));
}

/*
 * Does the last scan's file hold together, past its header: records in
 * order of range, each with all its hashes, to the file's end?  Read
 * before the scan (which may trust what it holds), and left at its first
 * record.  The file isn't synced, and a token from before a restart names
 * no scan, so a file that doesn't is only discarded.
 */
static bool
roots_check(RootScan *rs)
{
	struct stat st;
	off_t		at = sizeof(RootsHeader);
	uint32		last = PG_UINT32_MAX;
	RootsRange	rec;

	if (fstat(fileno(rs->old), &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", rs->old_path)));
	while (at < st.st_size)
	{
		if (st.st_size - at < (off_t) sizeof(rec) ||
			fseeko(rs->old, at, SEEK_SET) != 0 ||
			fread(&rec, 1, sizeof(rec), rs->old) != sizeof(rec) ||
			(last != PG_UINT32_MAX && rec.range <= last) ||
			rec.range == PG_UINT32_MAX ||
			(st.st_size - at - (off_t) sizeof(rec)) / ROOTS_ENTRY < rec.n)
			return false;
		last = rec.range;
		at += sizeof(rec) + (off_t) rec.n * ROOTS_ENTRY;
	}
	if (fseeko(rs->old, sizeof(RootsHeader), SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file \"%s\": %m", rs->old_path)));
	return true;
}

/* The generation of the settled file holding a value tree's root (0: none). */
static uint32
root_epoch(const uint8 *hash)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyHash	h;

	if (!PROLLY_HAS(cs, epoch_of))
		return 0;
	memset(&h, 0, sizeof(h));
	h.len = ROOTS_HASH_LEN;
	memcpy(h.bytes, hash, ROOTS_HASH_LEN);
	return PROLLY_CALL(cs, epoch_of,
					   PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace),
					   &h);
}

/* An entry (a hash and its generation) of this scan's file, noted. */
static void
note_all(RootScan *rs, const uint8 *entry)
{
	if (rs->nall == rs->maxall)
	{
		rs->maxall = Max(64, rs->maxall * 2);
		rs->all = rs->all ?
			repalloc_huge(rs->all, (Size) rs->maxall * ROOTS_ENTRY) :
			MemoryContextAllocHuge(CurrentMemoryContext,
								   (Size) rs->maxall * ROOTS_ENTRY);
	}
	memcpy(rs->all + (Size) rs->nall++ * ROOTS_ENTRY, entry, ROOTS_ENTRY);
}

/* A hash pointer's hash, found: a root, and one of the range's. */
static void
root_found(RootScan *rs, const uint8 *hash)
{
	ProllyHash	h;

	h.len = ROOTS_HASH_LEN;
	memcpy(h.bytes, hash, ROOTS_HASH_LEN);
	rs->visit(&h, rs->arg);
	if (rs->out == NULL || !rs->reading)
		return;
	if (rs->nfound == rs->maxfound)
	{
		rs->maxfound = Max(64, rs->maxfound * 2);
		rs->found = rs->found ?
			repalloc_huge(rs->found, (Size) rs->maxfound * ROOTS_ENTRY) :
			MemoryContextAllocHuge(CurrentMemoryContext,
								   (Size) rs->maxfound * ROOTS_ENTRY);
	}
	memcpy(rs->found + (Size) rs->nfound++ * ROOTS_ENTRY, hash,
		   ROOTS_HASH_LEN);
}

static int
roots_hash_cmp(const void *a, const void *b)
{
	return memcmp(a, b, ROOTS_HASH_LEN);
}

static int
roots_epoch_cmp(const void *a, const void *b)
{
	uint32		x;
	uint32		y;

	memcpy(&x, (const uint8 *) a + ROOTS_HASH_LEN, sizeof(x));
	memcpy(&y, (const uint8 *) b + ROOTS_HASH_LEN, sizeof(y));
	if (x != y)
		return x < y ? -1 : 1;
	return memcmp(a, b, ROOTS_HASH_LEN);
}

/* The range read: its hashes in this scan's file, each once. */
static void
range_done(RootScan *rs)
{
	RootsRange	rec;
	uint32		n = 0;

	if (rs->out == NULL || !rs->reading || rs->nfound == 0)
		return;
	qsort(rs->found, rs->nfound, ROOTS_ENTRY, roots_hash_cmp);
	for (uint32 i = 0; i < rs->nfound; i++)
	{
		uint8	   *h = rs->found + (Size) i * ROOTS_ENTRY;
		uint32		epoch;

		if (n > 0 &&
			memcmp(rs->found + (Size) (n - 1) * ROOTS_ENTRY, h,
				   ROOTS_HASH_LEN) == 0)
			continue;
		memmove(rs->found + (Size) n * ROOTS_ENTRY, h, ROOTS_HASH_LEN);
		epoch = root_epoch(h);
		memcpy(rs->found + (Size) n * ROOTS_ENTRY + ROOTS_HASH_LEN, &epoch,
			   sizeof(epoch));
		note_all(rs, rs->found + (Size) n * ROOTS_ENTRY);
		n++;
	}
	rec.range = rs->range;
	rec.n = n;
	roots_write(rs, &rec, sizeof(rec));
	roots_write(rs, rs->found, (Size) n * ROOTS_ENTRY);
	rs->nfound = 0;
}

/*
 * A range of the page store, read (changed), or not: then what the last
 * scan found in it holds still, roots again and this scan's.
 */
static void
scan_range(uint32 range, bool changed, void *arg)
{
	RootScan   *rs = arg;
	uint8		entry[ROOTS_ENTRY];

	range_done(rs);
	rs->range = range;
	rs->reading = changed;
	/* (a retire's: what the ranges unchanged hold, by the file by generation) */
	if (changed || rs->retire_lo > 0)
		return;
	if (rs->old == NULL)
		elog(ERROR, "a range of a page store unchanged since a scan whose roots aren't known");
	while (rs->old_at.range < range)
	{
		if (fseeko(rs->old, (off_t) rs->old_at.n * ROOTS_ENTRY,
				   SEEK_CUR) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in file \"%s\": %m",
							rs->old_path)));
		roots_next(rs);
	}
	if (rs->old_at.range != range)
		return;
	if (rs->out != NULL)
		roots_write(rs, &rs->old_at, sizeof(rs->old_at));
	for (uint32 i = 0; i < rs->old_at.n; i++)
	{
		uint32		epoch;

		roots_read(rs, entry, sizeof(entry));
		/* (one no settled file held when found: perhaps one does now) */
		memcpy(&epoch, entry + ROOTS_HASH_LEN, sizeof(epoch));
		if (epoch == 0)
		{
			epoch = root_epoch(entry);
			memcpy(entry + ROOTS_HASH_LEN, &epoch, sizeof(epoch));
		}
		if (rs->out != NULL)
		{
			roots_write(rs, entry, sizeof(entry));
			note_all(rs, entry);
		}
		root_found(rs, entry);
	}
	rs->old_at.n = 0;
	roots_next(rs);
}

/* Visit what bytes [start, stop) of a page spell hash pointers. */
static void
scan_bytes(RootScan *rs, const char *start, const char *stop)
{
	const char *p = start;
	const char *end;

	if (stop - start < (ptrdiff_t) (VARHDRSZ_EXTERNAL + sizeof(varatt_external_hash)))
		return;
	end = stop - (VARHDRSZ_EXTERNAL + sizeof(varatt_external_hash)) + 1;
	while (p < end && (p = memchr(p, rs->head[0], end - p)) != NULL)
	{
		if ((uint8) p[1] == rs->head[1])
		{
			varatt_external_hash ptr;

			memcpy(&ptr, p + VARHDRSZ_EXTERNAL, sizeof(ptr));
			root_found(rs, ptr.va_hash);
		}
		p++;
	}
}

/*
 * A page's items, dead or alive, as long as they have storage: not its
 * free space, where a pruned tuple's bytes may linger.  A page whose header
 * doesn't hold together is read whole.
 */
static void
scan_page(const char *page, void *arg)
{
	RootScan   *rs = arg;
	PageHeader	ph = (PageHeader) page;
	int			nitems;

	if (ph->pd_lower < SizeOfPageHeaderData || ph->pd_lower > ph->pd_upper ||
		ph->pd_upper > ph->pd_special || ph->pd_special > BLCKSZ)
	{
		scan_bytes(rs, page + SizeOfPageHeaderData, page + BLCKSZ);
		return;
	}
	nitems = (ph->pd_lower - SizeOfPageHeaderData) / sizeof(ItemIdData);
	for (int i = 0; i < nitems; i++)
	{
		ItemId		iid = &ph->pd_linp[i];
		unsigned	off = ItemIdGetOffset(iid);
		unsigned	len = ItemIdGetLength(iid);

		if (!ItemIdHasStorage(iid))
			continue;
		if (off < ph->pd_upper || off + len > ph->pd_special)
		{
			scan_bytes(rs, page + SizeOfPageHeaderData, page + BLCKSZ);
			return;
		}
		scan_bytes(rs, page + off, page + off + len);
	}
}

/* the file by generation's header, and its entries for each generation */
typedef struct EpochsHeader
{
	uint32		magic;
	uint32		version;
	uint64		token;
	uint32		nepochs;
} EpochsHeader;

typedef struct EpochsEntry
{
	uint32		epoch;
	uint32		n;
} EpochsEntry;

/*
 * Write the file by generation of what this scan wrote (rs->all), with
 * token, aside then renamed over the old one.
 */
static void
epochs_write(RootScan *rs, const char *path, uint64 token)
{
	char	   *tmp = psprintf("%s.tmp", path);
	FILE	   *f;
	EpochsHeader h = {EPOCHS_MAGIC, 1, token, 0};
	EpochsEntry *dir;
	int			ndir = 0;

	if (rs->nall > 1)
		qsort(rs->all, rs->nall, ROOTS_ENTRY, roots_epoch_cmp);
	dir = palloc(sizeof(EpochsEntry) * Max(rs->nall, 1));
	for (uint64 i = 0; i < rs->nall; i++)
	{
		uint32		epoch;

		memcpy(&epoch, rs->all + (Size) i * ROOTS_ENTRY + ROOTS_HASH_LEN,
			   sizeof(epoch));
		if (ndir == 0 || dir[ndir - 1].epoch != epoch)
		{
			dir[ndir].epoch = epoch;
			dir[ndir++].n = 0;
		}
		dir[ndir - 1].n++;
	}
	h.nepochs = ndir;
	f = AllocateFile(tmp, PG_BINARY_W);
	if (f == NULL ||
		fwrite(&h, 1, sizeof(h), f) != sizeof(h) ||
		fwrite(dir, sizeof(EpochsEntry), ndir, f) != (size_t) ndir)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmp)));
	for (uint64 i = 0; i < rs->nall; i++)
	{
		if (fwrite(rs->all + (Size) i * ROOTS_ENTRY, 1, ROOTS_HASH_LEN, f) !=
			ROOTS_HASH_LEN)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", tmp)));
	}
	if (FreeFile(f) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmp)));
	if (rename(tmp, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmp, path)));
	pfree(dir);
	pfree(tmp);
	rs->nall = 0;
}

/*
 * A retire's: visit the hashes the file by generation lists in generations
 * lo to hi, and those no settled file held when found (one may since);
 * false if the file isn't the one of the scan of token.
 */
static bool
epochs_visit(RootScan *rs, const char *path, uint64 token)
{
	FILE	   *f = AllocateFile(path, PG_BINARY_R);
	EpochsHeader h;
	EpochsEntry *dir;
	off_t		at;
	bool		ok;

	if (f == NULL)
		return false;
	ok = fread(&h, 1, sizeof(h), f) == sizeof(h) &&
		h.magic == EPOCHS_MAGIC && h.version == 1 && h.token == token &&
		h.nepochs < 0x10000000;
	if (!ok)
	{
		FreeFile(f);
		return false;
	}
	dir = palloc(sizeof(EpochsEntry) * Max(h.nepochs, 1));
	if (fread(dir, sizeof(EpochsEntry), h.nepochs, f) != h.nepochs)
	{
		pfree(dir);
		FreeFile(f);
		return false;
	}
	at = sizeof(h) + (off_t) h.nepochs * sizeof(EpochsEntry);
	for (uint32 i = 0; i < h.nepochs; i++)
	{
		uint8		hash[ROOTS_HASH_LEN];

		if (dir[i].epoch != 0 &&
			(dir[i].epoch < rs->retire_lo || dir[i].epoch > rs->retire_hi))
		{
			at += (off_t) dir[i].n * ROOTS_HASH_LEN;
			continue;
		}
		if (fseeko(f, at, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in file \"%s\": %m", path)));
		for (uint32 j = 0; j < dir[i].n; j++)
		{
			ProllyHash	ph;

			if (fread(hash, 1, sizeof(hash), f) != sizeof(hash))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("file \"%s\" is corrupt", path)));
			ph.len = ROOTS_HASH_LEN;
			memcpy(ph.bytes, hash, ROOTS_HASH_LEN);
			rs->visit(&ph, rs->arg);
		}
		at += (off_t) dir[i].n * ROOTS_HASH_LEN;
	}
	pfree(dir);
	FreeFile(f);
	return true;
}

/*
 * A retire's scan of a page store: what changed since the last scan read,
 * and of the rest only the hashes of generations lo to hi, from the file
 * by generation; every block read if that file isn't the last scan's.
 * Neither file is written: the next scan reads again what this one did.
 */
static void
scan_store_retire(Oid spc, RootScan *rs)
{
	char	   *dir = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
	char	   *path = psprintf("%s/%s%u", dir, ROOTS_PREFIX, spc);
	char	   *epochs = psprintf("%s%s", path, EPOCHS_SUFFIX);
	FILE	   *f = AllocateFile(path, PG_BINARY_R);
	RootsHeader h;
	uint64		since = 0;
	uint64		token;
	uint64		nread;

	if (f != NULL)
	{
		if (fread(&h, 1, sizeof(h), f) == sizeof(h) &&
			h.magic == ROOTS_MAGIC && h.version == ROOTS_VERSION)
			since = h.token;
		FreeFile(f);
	}
	rs->old = NULL;
	rs->out = NULL;
	rs->reading = false;
	rs->nfound = 0;
	/* (the hashes of the ranges unchanged first, from the file by generation) */
	if (since != 0 && !epochs_visit(rs, epochs, since))
		since = 0;
	nread = bmap_scan_store(spc, MyDatabaseId, since, &token, scan_range,
							scan_page, rs);
	pgstat_count_prolly(PGSTAT_PROLLY_GC_PAGES_SCANNED, nread);
	pfree(epochs);
	pfree(path);
	pfree(dir);
}

/*
 * The database's page store in tablespace spc: what changed since the last
 * scan read, the rest's hash pointers taken from its file, and the file
 * written again.
 */
static void
scan_store(Oid spc, RootScan *rs)
{
	char	   *dir = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
	char	   *path = psprintf("%s/%s%u", dir, ROOTS_PREFIX, spc);
	char	   *tmp = psprintf("%s.tmp", path);
	RootsHeader h;
	uint64		since = 0;
	uint64		token;
	uint64		nread;

	rs->old_path = path;
	rs->out_path = tmp;
	rs->old_at.range = PG_UINT32_MAX;
	rs->old_at.n = 0;
	rs->old = AllocateFile(path, PG_BINARY_R);
	if (rs->old == NULL && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	if (rs->old != NULL)
	{
		if (fread(&h, 1, sizeof(h), rs->old) == sizeof(h) &&
			h.magic == ROOTS_MAGIC && h.version == ROOTS_VERSION &&
			roots_check(rs))
		{
			since = h.token;
			roots_next(rs);
		}
		else
		{
			FreeFile(rs->old);
			rs->old = NULL;
		}
	}
	rs->out = AllocateFile(tmp, PG_BINARY_W);
	if (rs->out == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp)));
	h.magic = ROOTS_MAGIC;
	h.version = ROOTS_VERSION;
	h.token = 0;
	roots_write(rs, &h, sizeof(h));
	rs->reading = false;
	rs->nfound = 0;
	rs->nall = 0;

	nread = bmap_scan_store(spc, MyDatabaseId, since, &token, scan_range,
							scan_page, rs);
	range_done(rs);
	pgstat_count_prolly(PGSTAT_PROLLY_GC_PAGES_SCANNED, nread);

	/* the token last, once the rest is written */
	h.token = token;
	if (fseeko(rs->out, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file \"%s\": %m", tmp)));
	roots_write(rs, &h, sizeof(h));
	if (FreeFile(rs->out) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmp)));
	rs->out = NULL;
	if (rs->old != NULL)
		FreeFile(rs->old);
	rs->old = NULL;
	{
		char	   *epochs = psprintf("%s%s", path, EPOCHS_SUFFIX);

		/* (the file by generation first: it names the token it goes with) */
		if (token != 0)
			epochs_write(rs, epochs, token);
		else if (unlink(epochs) != 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", epochs)));
		pfree(epochs);
	}
	if (token == 0)
	{
		/* (no store in the tablespace) */
		if (unlink(tmp) != 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", tmp)));
		if (unlink(path) != 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
	}
	else if (rename(tmp, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmp, path)));
	pfree(tmp);
	pfree(path);
	pfree(dir);
}

static void
roots_scan(uint32 lo, uint32 hi, ProllyChunkRefVisit visit, void *arg)
{
	RootScan	rs;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *spcs = NIL;
	ListCell   *lc;

	memset(&rs, 0, sizeof(rs));
	SET_VARTAG_EXTERNAL(rs.head, VARTAG_ONDISK_BLAKE3);
	rs.visit = visit;
	rs.arg = arg;
	rs.retire_lo = lo;
	rs.retire_hi = hi;

	/* every page's latest version on disk */
	FlushDatabaseBuffers(MyDatabaseId);

	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			spc = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		if (spc != GLOBALTABLESPACE_OID)
			spcs = lappend_oid(spcs, spc);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach(lc, spcs)
	{
		if (lo > 0)
			scan_store_retire(lfirst_oid(lc), &rs);
		else
			scan_store(lfirst_oid(lc), &rs);
	}
	list_free(spcs);
	if (rs.found)
		pfree(rs.found);
	if (rs.all)
		pfree(rs.all);
}

void
prolly_toast_roots(ProllyChunkRefVisit visit, void *arg)
{
	roots_scan(0, 0, visit, arg);
}

/*
 * The same for a retire of generations lo to hi: of the blocks not written
 * since the last scan, only the hashes in those generations (and those no
 * settled file held when found).
 */
void
prolly_toast_roots_in(uint32 lo, uint32 hi, ProllyChunkRefVisit visit,
					  void *arg)
{
	Assert(lo > 0);
	roots_scan(lo, hi, visit, arg);
}

/* Visit what a WAL record's bytes spell as hash pointers. */
static void
scan_record(RootScan *rs, XLogReaderState *xlr, char *page)
{
	/* (the chunk store's own records hold chunks, which no slot decodes) */
	if (XLogRecGetRmid(xlr) != RM_PROLLY_CHUNKS_ID)
	{
		char	   *data = XLogRecGetData(xlr);

		scan_bytes(rs, data, data + XLogRecGetDataLen(xlr));
	}
	for (int id = 0; id <= XLogRecMaxBlockId(xlr); id++)
	{
		if (!XLogRecHasBlockRef(xlr, id))
			continue;
		if (XLogRecGetRmid(xlr) != RM_PROLLY_CHUNKS_ID &&
			XLogRecHasBlockData(xlr, id))
		{
			Size		len;
			char	   *data = XLogRecGetBlockData(xlr, id, &len);

			scan_bytes(rs, data, data + len);
		}
		if (XLogRecHasBlockImage(xlr, id))
		{
			if (!RestoreBlockImage(xlr, id, page))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("%s", xlr->errormsg_buf)));
			scan_bytes(rs, page, page + BLCKSZ);
		}
	}
}

void
prolly_toast_wal_roots(XLogRecPtr from, ProllyChunkRefVisit visit, void *arg)
{
	RootScan	rs;
	XLogRecPtr	end;
	XLogRecPtr	first;
	XLogReaderState *xlr;
	PGIOAlignedBlock page;
	char	   *errormsg;

	if (!XLogRecPtrIsValid(from))
		return;
	memset(&rs, 0, sizeof(rs));
	SET_VARTAG_EXTERNAL(rs.head, VARTAG_ONDISK_BLAKE3);
	rs.visit = visit;
	rs.arg = arg;

	/* to the end of the WAL as of now, flushed */
	end = GetXLogInsertRecPtr();
	XLogFlush(end);
	end = GetFlushRecPtr(NULL);
	if (from >= end)
		return;

	xlr = XLogReaderAllocate(wal_segment_size, NULL,
							 XL_ROUTINE(.page_read = read_local_xlog_page,
										.segment_open = wal_segment_open,
										.segment_close = wal_segment_close),
							 NULL);
	if (xlr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/*
	 * (On an error, the segment open is closed as the memory context the
	 * reader is in is reset: wal_segment_open() registers for it.)
	 */
	first = XLogFindNextRecord(xlr, from, &errormsg);
	if (!XLogRecPtrIsValid(first))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not find a valid record after %X/%08X",
						LSN_FORMAT_ARGS(from)),
				 errormsg ? errdetail_internal("%s", errormsg) : 0));
	XLogBeginRead(xlr, first);
	while (xlr->EndRecPtr < end)
	{
		if (XLogReadRecord(xlr, &errormsg) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read WAL at %X/%08X",
							LSN_FORMAT_ARGS(xlr->EndRecPtr)),
					 errormsg ? errdetail_internal("%s", errormsg) : 0));
		INJECTION_POINT("prolly-wal-roots-read", NULL);
		CHECK_FOR_INTERRUPTS();
		scan_record(&rs, xlr, page.data);
	}
	XLogReaderFree(xlr);
}

/* A versioned table's value moved out of line by the toaster. */
static varlena *
chunkstore_save(Relation rel, Datum value, Oid typid)
{
	return prolly_toast_store(value, typid);
}

/*
 * A hash pointer a versioned table's row is given: held as it is if its
 * form is the one a value of the column's type is stored in (a pointer of
 * bytes is, whatever the type: a value whose conversion wouldn't give it
 * back is stored so), its tree kept.
 */
static bool
chunkstore_reference(Relation rel, varlena *pointer, Oid typid)
{
	ProllyHash	root;
	uint64		len;
	int			form;

	ensure_env();
	form = pointer_root(pointer, &root, &len);
	if (form != toast_form(typid) &&
		!(form == PROLLY_TOAST_FORM_BYTES && toast_form(typid) == PROLLY_TOAST_FORM_UTF8) &&
		!(form == PROLLY_TOAST_FORM_JSON_TREE && toast_form(typid) == PROLLY_TOAST_FORM_JSONB))
		return false;
	PROLLY_CALL(env.chunk_store, keep, env.chunks, &root);
	return true;
}

const ToastDestinationRoutine prolly_toast_chunkstore = {
	.save = chunkstore_save,
	.reference = chunkstore_reference,
	.fetch = chunkstore_fetch,
};
