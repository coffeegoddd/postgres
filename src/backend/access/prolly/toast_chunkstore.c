/*-------------------------------------------------------------------------
 *
 * toast_chunkstore.c
 *	  The TOAST destination of SHA-256 hash pointers: value trees in the
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
 * keeps a hash pointer a row is given as it is.
 *
 * A tree is the chunk store's, not a row's: nothing deletes it with the
 * rows that name it.  A collection of the store keeps every tree a hash
 * pointer on a page names (prolly_toast_roots()), and each tree stored is
 * told to the store's keep(), for a collection running meanwhile.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/toast_chunkstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

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
#include "miscadmin.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "varatt.h"

/* the trees' environment, the current database's */
static ProllyTreeEnv env;
static const ProllyTreeRoutine *tree;
static bool env_ready = false;
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
 * The form a value of type typid is stored in, if it can be: its bytes,
 * unless converting them to the canonical bytes changes more than their
 * form (text-like in another encoding than UTF-8, jsonb).
 */
static int
toast_form(Oid typid)
{
	Oid			base = getBaseType(typid);

	if (base == JSONBOID)
		return PROLLY_TOAST_FORM_JSONB;
	/* (SQL_ASCII's bytes are no encoding's: kept as they are) */
	if ((base == TEXTOID || base == VARCHAROID || base == BPCHAROID) &&
		GetDatabaseEncoding() != PG_UTF8 &&
		GetDatabaseEncoding() != PG_SQL_ASCII)
		return PROLLY_TOAST_FORM_UTF8;
	return PROLLY_TOAST_FORM_BYTES;
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
		elog(ERROR, "a value tree's root isn't a SHA-256 hash pointer's");
	PROLLY_CALL(env.chunk_store, keep, env.chunks, &root);

	p.va_rawsize = VARSIZE_ANY_EXHDR(plain) + VARHDRSZ;
	p.va_extinfo = (uint32) len | ((uint32) form << VARLENA_EXTSIZE_BITS);
	memcpy(p.va_hash, root.bytes, sizeof(p.va_hash));
	result = (varlena *) palloc(VARHDRSZ_EXTERNAL + sizeof(p));
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK_SHA256);
	memcpy(VARDATA_EXTERNAL(result), &p, sizeof(p));

	pfree(ref.data);
	pfree(canonical.data);
	if (plain != v)
		pfree(plain);
	return result;
}

int
prolly_toast_reference(const varlena *pointer, StringInfo ref)
{
	varatt_external_hash p;
	ProllyHash	root;

	Assert(VARATT_IS_EXTERNAL_ONDISK_HASH(pointer));
	ensure_env();
	VARATT_EXTERNAL_GET_POINTER(p, pointer);
	root.len = sizeof(p.va_hash);
	memcpy(root.bytes, p.va_hash, sizeof(p.va_hash));
	PROLLY_CALL(tree, value_reference, &root,
				VARATT_EXTINFO_GET_EXTSIZE(p.va_extinfo), ref);
	return p.va_extinfo >> VARLENA_EXTSIZE_BITS;
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

/* the bytes a hash pointer starts with */
typedef struct RootScan
{
	uint8		head[VARHDRSZ_EXTERNAL];
	ProllyChunkRefVisit visit;
	void	   *arg;
} RootScan;

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
			ProllyHash	h;

			memcpy(&ptr, p + VARHDRSZ_EXTERNAL, sizeof(ptr));
			h.len = sizeof(ptr.va_hash);
			memcpy(h.bytes, ptr.va_hash, sizeof(ptr.va_hash));
			rs->visit(&h, rs->arg);
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

void
prolly_toast_roots(ProllyChunkRefVisit visit, void *arg)
{
	RootScan	rs;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *spcs = NIL;
	ListCell   *lc;

	SET_VARTAG_EXTERNAL(rs.head, VARTAG_ONDISK_SHA256);
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
		(void) bmap_scan_store(lfirst_oid(lc), MyDatabaseId, scan_page, &rs);
	list_free(spcs);
}

/* A versioned table's value moved out of line by the toaster. */
static varlena *
chunkstore_save(Relation rel, Datum value, Oid typid)
{
	return prolly_toast_store(value, typid);
}

const ToastDestinationRoutine prolly_toast_chunkstore = {
	.save = chunkstore_save,
	.fetch = chunkstore_fetch,
};
