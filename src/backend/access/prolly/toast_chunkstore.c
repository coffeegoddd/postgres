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

static void
ensure_env(void)
{
	if (env_ready)
		return;
	if (!OidIsValid(MyDatabaseId))
		elog(ERROR, "values by content need a database");
	tree = prolly_tree_lookup("cow");
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

	Assert(VARATT_IS_EXTERNAL_ONDISK_HASH(pointer));
	ensure_env();
	if (pointer_root(pointer, &root, len) != canonical_form(typid))
		return false;

	/* the trees of history's value store are these, in this store */
	if (values->tree != tree || values->env->chunk_store != env.chunk_store ||
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
	PROLLY_CALL(tree, value_reference, &root, len, ref);
	return form;
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
 * hashes, and the hashes, each once.  The page store's token says whether
 * it still holds.  It is not synced: a crash makes every token stale.
 */
#define ROOTS_PREFIX		"prolly_page_roots_"	/* and the tablespace */
#define ROOTS_MAGIC			0x50524F54	/* "PROT" */
#define ROOTS_VERSION		1
#define ROOTS_HASH_LEN		sizeof(((varatt_external_hash *) NULL)->va_hash)

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
	uint8	   *found;			/* its hashes, ROOTS_HASH_LEN bytes each */
	uint32		nfound;
	uint32		maxfound;
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
			repalloc_huge(rs->found, (Size) rs->maxfound * ROOTS_HASH_LEN) :
			MemoryContextAllocHuge(CurrentMemoryContext,
								   (Size) rs->maxfound * ROOTS_HASH_LEN);
	}
	memcpy(rs->found + (Size) rs->nfound++ * ROOTS_HASH_LEN, hash,
		   ROOTS_HASH_LEN);
}

static int
roots_hash_cmp(const void *a, const void *b)
{
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
	qsort(rs->found, rs->nfound, ROOTS_HASH_LEN, roots_hash_cmp);
	for (uint32 i = 0; i < rs->nfound; i++)
	{
		uint8	   *h = rs->found + (Size) i * ROOTS_HASH_LEN;

		if (n > 0 &&
			memcmp(rs->found + (Size) (n - 1) * ROOTS_HASH_LEN, h,
				   ROOTS_HASH_LEN) == 0)
			continue;
		memmove(rs->found + (Size) n++ * ROOTS_HASH_LEN, h, ROOTS_HASH_LEN);
	}
	rec.range = rs->range;
	rec.n = n;
	roots_write(rs, &rec, sizeof(rec));
	roots_write(rs, rs->found, (Size) n * ROOTS_HASH_LEN);
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
	uint8		hash[ROOTS_HASH_LEN];

	range_done(rs);
	rs->range = range;
	rs->reading = changed;
	if (changed)
		return;
	if (rs->old == NULL)
		elog(ERROR, "a range of a page store unchanged since a scan whose roots aren't known");
	while (rs->old_at.range < range)
	{
		if (fseeko(rs->old, (off_t) rs->old_at.n * ROOTS_HASH_LEN,
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
		roots_read(rs, hash, sizeof(hash));
		if (rs->out != NULL)
			roots_write(rs, hash, sizeof(hash));
		root_found(rs, hash);
	}
	rs->old_at.n = 0;
	roots_next(rs);
}

/* Visit what bytes [start, stop) of a page spell hash pointers. */
static void
scan_bytes(RootScan *rs, const char *start, const char *stop)
{
	const char *p = start;
	const char *end = stop -
		(VARHDRSZ_EXTERNAL + sizeof(varatt_external_hash)) + 1;

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
			h.magic == ROOTS_MAGIC && h.version == ROOTS_VERSION)
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

void
prolly_toast_roots(ProllyChunkRefVisit visit, void *arg)
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
		scan_store(lfirst_oid(lc), &rs);
	list_free(spcs);
	if (rs.found)
		pfree(rs.found);
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
		!(form == PROLLY_TOAST_FORM_BYTES && toast_form(typid) == PROLLY_TOAST_FORM_UTF8))
		return false;
	PROLLY_CALL(env.chunk_store, keep, env.chunks, &root);
	return true;
}

const ToastDestinationRoutine prolly_toast_chunkstore = {
	.save = chunkstore_save,
	.reference = chunkstore_reference,
	.fetch = chunkstore_fetch,
};
