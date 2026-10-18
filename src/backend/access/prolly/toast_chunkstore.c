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
 * IDENTIFICATION
 *	  src/backend/access/prolly/toast_chunkstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/prolly_canon.h"
#include "access/prolly_datum.h"
#include "access/prolly_toast.h"
#include "access/prolly_tree.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "varatt.h"

/* the trees' environment, the current database's */
static ProllyTreeEnv env;
static const ProllyTreeRoutine *tree;
static Oid	env_database = InvalidOid;
static ProllyDatumCodec *jsonb_codec;

static void
ensure_env(void)
{
	if (env_database == MyDatabaseId)
		return;
	tree = prolly_tree_lookup("cow");
	prolly_folded_tree_env(&env);
	env_database = MyDatabaseId;
}

static ProllyDatumCodec *
jsonb(void)
{
	if (jsonb_codec == NULL)
		jsonb_codec = prolly_datum_codec(JSONBOID, -1, TopMemoryContext);
	return jsonb_codec;
}

/* Is typid text, varchar or bpchar (a domain over one included)? */
static bool
text_like(Oid typid)
{
	Oid			base = getBaseType(typid);

	return base == TEXTOID || base == VARCHAROID || base == BPCHAROID;
}

varlena *
prolly_toast_store(Datum value, Oid typid)
{
	varlena    *v = (varlena *) DatumGetPointer(value);
	varlena    *plain = v;
	const char *data;
	Size		len;
	char	   *converted = NULL;
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
	if (getBaseType(typid) == JSONBOID)
	{
		form = PROLLY_TOAST_FORM_JSONB;
		prolly_datum_encode(jsonb(), PointerGetDatum(plain), &canonical);
		data = canonical.data;
		len = canonical.len;
	}
	else if (text_like(typid) && GetDatabaseEncoding() != PG_UTF8)
	{
		form = PROLLY_TOAST_FORM_UTF8;
		converted = pg_server_to_any(data, len, PG_UTF8);
		if (converted != data)
		{
			data = converted;
			len = strlen(converted);
		}
		else
			converted = NULL;
	}
	if (len > VARLENA_EXTSIZE_MASK)
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

	p.va_rawsize = VARSIZE_ANY_EXHDR(plain) + VARHDRSZ;
	p.va_extinfo = (uint32) len | ((uint32) form << VARLENA_EXTSIZE_BITS);
	memcpy(p.va_hash, root.bytes, sizeof(p.va_hash));
	result = (varlena *) palloc(VARHDRSZ_EXTERNAL + sizeof(p));
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK_SHA256);
	memcpy(VARDATA_EXTERNAL(result), &p, sizeof(p));

	pfree(ref.data);
	pfree(canonical.data);
	if (converted)
		pfree(converted);
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

const ToastDestinationRoutine prolly_toast_chunkstore = {
	.fetch = chunkstore_fetch,
};
