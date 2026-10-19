/*-------------------------------------------------------------------------
 *
 * prolly_datum.c
 *	  Canonical encoding of single values; see prolly_datum.h.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_datum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_datum.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

typedef enum CodecKind
{
	CODEC_INT2,
	CODEC_INT4,
	CODEC_INT8,
	CODEC_BOOL,
	CODEC_TEXT,					/* text, varchar, bpchar in a UTF-8 database */
	CODEC_ARRAY,
	CODEC_COMPOSITE,
	CODEC_ANYARRAY,
	CODEC_JSONB,
	CODEC_SEND,
} CodecKind;

/* What an array codec needs to know about its element type. */
typedef struct ElemInfo
{
	Oid			type;
	int16		len;
	bool		byval;
	char		align;
	ProllyDatumCodec *codec;
} ElemInfo;

struct ProllyDatumCodec
{
	CodecKind	kind;
	Oid			type;			/* the base type */
	int32		typmod;
	MemoryContext mcxt;

	/* CODEC_SEND */
	FmgrInfo	send;
	FmgrInfo	recv;
	Oid			ioparam;

	/* CODEC_ARRAY */
	ElemInfo	elem;

	/* CODEC_COMPOSITE */
	TupleDesc	tupdesc;
	ProllyDatumCodec **attcodecs;	/* NULL for dropped attributes */

	/* CODEC_ANYARRAY: element codecs built so far */
	List	   *anyarray_elems;	/* of ElemInfo * */
};

/* ------------------------------------------------------------------------
 * Varints
 * ------------------------------------------------------------------------
 */

void
prolly_put_varint(StringInfo out, uint64 v)
{
	char		buf[10];
	int			n = 0;

	do
	{
		uint8		b = v & 0x7f;

		v >>= 7;
		buf[n++] = (char) (v ? (b | 0x80) : b);
	} while (v);
	appendBinaryStringInfo(out, buf, n);
}

uint64
prolly_get_varint(const char **p, const char *end)
{
	uint64		v = 0;
	int			shift = 0;

	for (;;)
	{
		uint8		b;

		if (*p >= end || shift > 63)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid varint in canonical prolly value")));
		b = (uint8) *(*p)++;
		v |= (uint64) (b & 0x7f) << shift;
		if ((b & 0x80) == 0)
			return v;
		shift += 7;
	}
}

static inline uint32
zigzag32(int32 v)
{
	return ((uint32) v << 1) ^ (uint32) (v >> 31);
}

static inline int32
unzigzag32(uint32 v)
{
	return (int32) (v >> 1) ^ -(int32) (v & 1);
}

static void
put_be(StringInfo out, uint64 v, int nbytes)
{
	char		buf[8];

	for (int i = nbytes - 1; i >= 0; i--)
	{
		buf[i] = (char) (v & 0xff);
		v >>= 8;
	}
	appendBinaryStringInfo(out, buf, nbytes);
}

static uint64
get_be(const char *p, int nbytes)
{
	uint64		v = 0;

	for (int i = 0; i < nbytes; i++)
		v = (v << 8) | (uint8) p[i];
	return v;
}

static void
bad_value(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid canonical prolly value: %s", what)));
}

/* Read varint length, then check that many bytes remain. */
static const char *
get_chunk(const char **p, const char *end, int *len)
{
	uint64		n = prolly_get_varint(p, end);
	const char *start = *p;

	if (n > (uint64) (end - *p))
		bad_value("length runs past the end");
	*p += n;
	*len = (int) n;
	return start;
}

/* ------------------------------------------------------------------------
 * Send and receive with the client encoding fixed to UTF-8
 * ------------------------------------------------------------------------
 */

static bytea *
send_utf8(FmgrInfo *send, Datum value)
{
	int			old = SetClientEncodingOverride(PG_UTF8);
	bytea	   *result;

	PG_TRY();
	{
		result = SendFunctionCall(send, value);
	}
	PG_FINALLY();
	{
		SetClientEncodingOverride(old);
	}
	PG_END_TRY();
	return result;
}

static Datum
recv_utf8(ProllyDatumCodec *c, const char *data, int len)
{
	int			old = SetClientEncodingOverride(PG_UTF8);
	StringInfoData buf;
	Datum		result;

	initReadOnlyStringInfo(&buf, unconstify(char *, data), len);
	PG_TRY();
	{
		result = ReceiveFunctionCall(&c->recv, &buf, c->ioparam, c->typmod);
	}
	PG_FINALLY();
	{
		SetClientEncodingOverride(old);
	}
	PG_END_TRY();
	if (buf.cursor != buf.len)
		bad_value("receive function did not consume the value");
	return result;
}

/* ------------------------------------------------------------------------
 * Codec construction
 * ------------------------------------------------------------------------
 */

static void
elem_info_init(ElemInfo *ei, Oid elemtype, int32 typmod, MemoryContext mcxt)
{
	ei->type = elemtype;
	get_typlenbyvalalign(elemtype, &ei->len, &ei->byval, &ei->align);
	ei->codec = prolly_datum_codec(elemtype, typmod, mcxt);
}

ProllyDatumCodec *
prolly_datum_codec(Oid typid, int32 typmod, MemoryContext mcxt)
{
	ProllyDatumCodec *c = MemoryContextAllocZero(mcxt, sizeof(ProllyDatumCodec));
	Oid			base = getBaseTypeAndTypmod(typid, &typmod);
	Oid			elemtype;

	c->type = base;
	c->typmod = typmod;
	c->mcxt = mcxt;

	switch (base)
	{
		case INT2OID:
			c->kind = CODEC_INT2;
			return c;
		case INT4OID:
			c->kind = CODEC_INT4;
			return c;
		case INT8OID:
			c->kind = CODEC_INT8;
			return c;
		case BOOLOID:
			c->kind = CODEC_BOOL;
			return c;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			if (GetDatabaseEncoding() == PG_UTF8)
			{
				c->kind = CODEC_TEXT;
				return c;
			}
			break;
		case ANYARRAYOID:
			c->kind = CODEC_ANYARRAY;
			return c;
		case JSONBOID:
			{
				Oid			recv;

				/* jsonb_recv, for canonical bytes of jsonb_send's version */
				c->kind = CODEC_JSONB;
				getTypeBinaryInputInfo(base, &recv, &c->ioparam);
				fmgr_info_cxt(recv, &c->recv, mcxt);
				return c;
			}
		case RECORDOID:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prolly storage cannot store values of an anonymous record type")));
	}

	elemtype = get_element_type(base);
	if (OidIsValid(elemtype))
	{
		c->kind = CODEC_ARRAY;
		elem_info_init(&c->elem, elemtype, typmod, mcxt);
		return c;
	}

	if (get_typtype(base) == TYPTYPE_COMPOSITE)
	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		c->kind = CODEC_COMPOSITE;
		c->tupdesc = lookup_rowtype_tupdesc_copy(base, -1);
		c->attcodecs = palloc0(sizeof(ProllyDatumCodec *) * c->tupdesc->natts);
		MemoryContextSwitchTo(old);
		for (int i = 0; i < c->tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(c->tupdesc, i);

			if (!att->attisdropped)
				c->attcodecs[i] = prolly_datum_codec(att->atttypid, att->atttypmod,
													 mcxt);
		}
		return c;
	}

	{
		Oid			send;
		Oid			recv;
		bool		varlena;

		c->kind = CODEC_SEND;
		getTypeBinaryOutputInfo(base, &send, &varlena);
		getTypeBinaryInputInfo(base, &recv, &c->ioparam);
		fmgr_info_cxt(send, &c->send, mcxt);
		fmgr_info_cxt(recv, &c->recv, mcxt);
	}
	return c;
}

/* ------------------------------------------------------------------------
 * Arrays, composites and anyarray
 * ------------------------------------------------------------------------
 */

static void
put_null_bitmap(StringInfo out, const bool *nulls, int n)
{
	for (int i = 0; i < n; i += 8)
	{
		uint8		b = 0;

		for (int j = 0; j < 8 && i + j < n; j++)
			if (nulls[i + j])
				b |= 1 << j;
		appendStringInfoChar(out, (char) b);
	}
}

static void
get_null_bitmap(const char **p, const char *end, bool *nulls, int n)
{
	int			nbytes = (n + 7) / 8;

	if (end - *p < nbytes)
		bad_value("null bitmap runs past the end");
	for (int i = 0; i < n; i++)
		nulls[i] = ((uint8) (*p)[i / 8] >> (i % 8)) & 1;
	*p += nbytes;
}

static void
put_value(ProllyDatumCodec *c, Datum value, StringInfo out)
{
	StringInfoData tmp;

	initStringInfo(&tmp);
	prolly_datum_encode(c, value, &tmp);
	prolly_put_varint(out, tmp.len);
	appendBinaryStringInfo(out, tmp.data, tmp.len);
	pfree(tmp.data);
}

static void
encode_array(ElemInfo *ei, Datum value, StringInfo out)
{
	ArrayType  *arr = DatumGetArrayTypeP(value);
	int			ndim = ARR_NDIM(arr);
	Datum	   *elems;
	bool	   *nulls;
	int			n;

	prolly_put_varint(out, ndim);
	if (ndim == 0)
		return;
	for (int d = 0; d < ndim; d++)
	{
		prolly_put_varint(out, ARR_DIMS(arr)[d]);
		prolly_put_varint(out, zigzag32(ARR_LBOUND(arr)[d]));
	}
	deconstruct_array(arr, ei->type, ei->len, ei->byval, ei->align,
					  &elems, &nulls, &n);
	put_null_bitmap(out, nulls, n);
	for (int i = 0; i < n; i++)
	{
		if (!nulls[i])
			put_value(ei->codec, elems[i], out);
	}
}

static Datum
decode_array(ElemInfo *ei, const char **p, const char *end)
{
	int			ndim = (int) prolly_get_varint(p, end);
	int			dims[MAXDIM];
	int			lbs[MAXDIM];
	int			n = 1;
	Datum	   *elems;
	bool	   *nulls;

	if (ndim == 0)
		return PointerGetDatum(construct_empty_array(ei->type));
	if (ndim < 0 || ndim > MAXDIM)
		bad_value("array has too many dimensions");
	for (int d = 0; d < ndim; d++)
	{
		dims[d] = (int) prolly_get_varint(p, end);
		lbs[d] = unzigzag32((uint32) prolly_get_varint(p, end));
		n *= dims[d];
	}
	elems = palloc(sizeof(Datum) * Max(n, 1));
	nulls = palloc(sizeof(bool) * Max(n, 1));
	get_null_bitmap(p, end, nulls, n);
	for (int i = 0; i < n; i++)
	{
		int			len;
		const char *data;

		if (nulls[i])
		{
			elems[i] = (Datum) 0;
			continue;
		}
		data = get_chunk(p, end, &len);
		elems[i] = prolly_datum_decode(ei->codec, data, len);
	}
	return PointerGetDatum(construct_md_array(elems, nulls, ndim, dims, lbs,
											  ei->type, ei->len, ei->byval,
											  ei->align));
}

static void
encode_composite(ProllyDatumCodec *c, Datum value, StringInfo out)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(value);
	HeapTupleData tuple;
	TupleDesc	desc = c->tupdesc;
	Datum	   *values = palloc(sizeof(Datum) * desc->natts);
	bool	   *isnull = palloc(sizeof(bool) * desc->natts);
	bool	   *nulls = palloc(sizeof(bool) * desc->natts);
	int			nlive = 0;
	int			n = 0;
	int			live;

	tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&tuple.t_self);
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = td;
	heap_deform_tuple(&tuple, desc, values, isnull);

	/* Live attributes, and how many up to the last non-NULL one. */
	for (int i = 0; i < desc->natts; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		nulls[nlive++] = isnull[i];
		if (!isnull[i])
			n = nlive;
	}

	prolly_put_varint(out, n);
	put_null_bitmap(out, nulls, n);
	live = 0;
	for (int i = 0; i < desc->natts && live < n; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		if (!isnull[i])
			put_value(c->attcodecs[i], values[i], out);
		live++;
	}
}

static Datum
decode_composite(ProllyDatumCodec *c, const char **p, const char *end)
{
	TupleDesc	desc = c->tupdesc;
	Datum	   *values = palloc0(sizeof(Datum) * desc->natts);
	bool	   *isnull = palloc(sizeof(bool) * desc->natts);
	bool	   *nulls = palloc(sizeof(bool) * Max(desc->natts, 1));
	int			n = (int) prolly_get_varint(p, end);
	int			live = 0;
	HeapTuple	tuple;

	if (n > desc->natts)
		bad_value("composite has too many attributes");
	get_null_bitmap(p, end, nulls, n);
	for (int i = 0; i < desc->natts; i++)
	{
		isnull[i] = true;
		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		if (live < n && !nulls[live])
		{
			int			len;
			const char *data = get_chunk(p, end, &len);

			values[i] = prolly_datum_decode(c->attcodecs[i], data, len);
			isnull[i] = false;
		}
		live++;
	}
	tuple = heap_form_tuple(desc, values, isnull);
	return heap_copy_tuple_as_datum(tuple, desc);
}

static void
put_name(StringInfo out, const char *name)
{
	const char *utf8 = pg_server_to_any(name, strlen(name), PG_UTF8);

	prolly_put_varint(out, strlen(utf8));
	appendStringInfoString(out, utf8);
}

static char *
get_name(const char **p, const char *end)
{
	int			len;
	const char *data = get_chunk(p, end, &len);

	return pg_any_to_server(pnstrdup(data, len), len, PG_UTF8);
}

static ElemInfo *
anyarray_elem(ProllyDatumCodec *c, Oid elemtype)
{
	ListCell   *lc;
	ElemInfo   *ei;
	MemoryContext old;

	foreach(lc, c->anyarray_elems)
	{
		ei = lfirst(lc);
		if (ei->type == elemtype)
			return ei;
	}
	ei = MemoryContextAlloc(c->mcxt, sizeof(ElemInfo));
	elem_info_init(ei, elemtype, -1, c->mcxt);
	old = MemoryContextSwitchTo(c->mcxt);
	c->anyarray_elems = lappend(c->anyarray_elems, ei);
	MemoryContextSwitchTo(old);
	return ei;
}

static void
encode_anyarray(ProllyDatumCodec *c, Datum value, StringInfo out)
{
	ArrayType  *arr = DatumGetArrayTypeP(value);
	Oid			elemtype = ARR_ELEMTYPE(arr);
	HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(elemtype));
	Form_pg_type typ;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", elemtype);
	typ = (Form_pg_type) GETSTRUCT(tup);
	put_name(out, get_namespace_name(typ->typnamespace));
	put_name(out, NameStr(typ->typname));
	ReleaseSysCache(tup);
	encode_array(anyarray_elem(c, elemtype), PointerGetDatum(arr), out);
}

static Datum
decode_anyarray(ProllyDatumCodec *c, const char **p, const char *end)
{
	char	   *nspname = get_name(p, end);
	char	   *typname = get_name(p, end);
	Oid			nsp = get_namespace_oid(nspname, false);
	Oid			elemtype = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
										   CStringGetDatum(typname),
										   ObjectIdGetDatum(nsp));

	if (!OidIsValid(elemtype))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s.%s\" does not exist", nspname, typname)));
	return decode_array(anyarray_elem(c, elemtype), p, end);
}

/* ------------------------------------------------------------------------
 * jsonb
 *
 * A version byte, then the document as jsonb stores it (after the varlena
 * header), its strings in UTF-8.  jsonb's format is deterministic (an
 * object's keys sorted and unique, an offset every JB_OFFSET_STRIDE
 * children, zeros for padding, numerics as numeric builds them), so equal
 * documents have equal bytes, and in a UTF-8 database a document's
 * canonical bytes are the ones it holds.  In another encoding a document
 * with a byte outside ASCII is rebuilt with its strings converted (which
 * can reorder its keys), going in and coming out.  Bytes decoded may come
 * from another server, so they are checked to be such a document first.
 *
 * The version byte is 2: 1 is jsonb_send's, its text, which jsonb's
 * canonical bytes were before; such bytes are still read.
 * ------------------------------------------------------------------------
 */

#define JSONB_CANONICAL_VERSION	2
#define JSONB_SEND_VERSION		1

/* Are s's len bytes all ASCII? */
static bool
all_ascii(const char *s, Size len)
{
	for (Size i = 0; i < len; i++)
		if (IS_HIGHBIT_SET(s[i]))
			return false;
	return true;
}

/*
 * jb rebuilt with every string, key or value, converted from the server's
 * encoding to UTF-8 (to_utf8) or back.  Built in a context of its own, so
 * only the result is left.  Two keys of an object that convert to the
 * same bytes are an error, not one key.
 */
static Jsonb *
jsonb_convert(Jsonb *jb, bool to_utf8)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly jsonb conversion",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	JsonbIterator *it = JsonbIteratorInit(&jb->root);
	JsonbInState state;
	JsonbIteratorToken tok;
	JsonbValue	v;
	Jsonb	   *result;

	memset(&state, 0, sizeof(state));
	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		bool		scalar = (tok == WJB_KEY || tok == WJB_VALUE ||
							  tok == WJB_ELEM);

		if (scalar && v.type == jbvString)
		{
			char	   *s = v.val.string.val;
			char	   *r = to_utf8 ?
				pg_server_to_any(s, v.val.string.len, PG_UTF8) :
				pg_any_to_server(s, v.val.string.len, PG_UTF8);

			if (r != s)
				v.val.string.len = strlen(r);
			v.val.string.val = r;
		}
		/* (a raw scalar's array says so in v) */
		pushJsonbValue(&state, tok,
					   scalar || (tok == WJB_BEGIN_ARRAY &&
								  v.val.array.rawScalar) ? &v : NULL);
		if (tok == WJB_BEGIN_OBJECT)
			state.parseState->unique_keys = true;
	}
	MemoryContextSwitchTo(old);
	result = JsonbValueToJsonb(state.result);
	MemoryContextDelete(cxt);
	return result;
}

static void
encode_jsonb(Datum value, StringInfo out)
{
	Jsonb	   *jb = DatumGetJsonbP(value);
	Jsonb	   *u = jb;

	if (GetDatabaseEncoding() != PG_UTF8 &&
		!all_ascii(VARDATA(jb), VARSIZE(jb) - VARHDRSZ))
		u = jsonb_convert(jb, true);
	appendStringInfoChar(out, JSONB_CANONICAL_VERSION);
	appendBinaryStringInfo(out, VARDATA(u), VARSIZE(u) - VARHDRSZ);
	if (u != jb)
		pfree(u);
	if ((Pointer) jb != DatumGetPointer(value))
		pfree(jb);
}

/*
 * The zeros padding doc[at, ...) to a 4-byte boundary of the document,
 * within end; their count.  (The document starts at one: its varlena
 * header is 4 bytes.)
 */
static uint32
jsonb_padding(const char *doc, uint32 at, uint32 end)
{
	uint32		pad = INTALIGN(at) - at;

	if (pad > end - at)
		bad_value("jsonb padding runs past its entry");
	for (uint32 i = 0; i < pad; i++)
		if (doc[at + i] != 0)
			bad_value("jsonb padding that isn't zeros");
	return pad;
}

/* Child i's entry of the container at doc + at. */
static JEntry
jsonb_entry(const char *doc, uint32 at, uint32 i)
{
	JEntry		e;

	memcpy(&e, doc + at + sizeof(uint32) + i * sizeof(JEntry), sizeof(e));
	return e;
}

/*
 * Check the container at doc[at, end) is one jsonb builds: entries within
 * it, an offset every JB_OFFSET_STRIDE children and lengths between,
 * keys sorted and unique, strings in UTF-8, numerics finite and as
 * numeric builds them, padding zeros, nothing after its last child.
 */
static void
check_jsonb_container(const char *doc, uint32 at, uint32 end, bool top)
{
	uint32		header;
	uint32		count;
	uint32		nchildren;
	uint32		data;
	uint32		prev = 0;		/* the previous child's end */
	uint32		key_start = 0;
	uint32		key_len = 0;
	bool		object;

	check_stack_depth();
	if (end - at < sizeof(uint32))
		bad_value("a jsonb container without a header");
	memcpy(&header, doc + at, sizeof(uint32));
	count = header & JB_CMASK;
	object = (header & JB_FOBJECT) != 0;
	if ((header & ~(JB_CMASK | JB_FSCALAR | JB_FOBJECT | JB_FARRAY)) != 0 ||
		object == ((header & JB_FARRAY) != 0))
		bad_value("a jsonb container neither an object nor an array");
	if ((header & JB_FSCALAR) != 0 && (!top || object || count != 1))
		bad_value("a jsonb scalar where a container belongs");
	nchildren = object ? count * 2 : count;
	if ((uint64) nchildren * sizeof(JEntry) > end - at - sizeof(uint32))
		bad_value("more jsonb entries than the container holds");
	data = at + sizeof(uint32) + nchildren * sizeof(JEntry);

	for (uint32 i = 0; i < nchildren; i++)
	{
		JEntry		e = jsonb_entry(doc, at, i);
		uint32		start = prev;
		uint32		stop;
		uint32		from = data + start;

		if (JBE_HAS_OFF(e) != (i % JB_OFFSET_STRIDE == 0))
			bad_value("a jsonb offset where a length belongs, or a length where an offset does");
		stop = JBE_HAS_OFF(e) ? JBE_OFFLENFLD(e) : start + JBE_OFFLENFLD(e);
		if (stop < start || stop > end - data)
			bad_value("a jsonb entry outside its container");
		if (object && i < count)
		{
			uint32		len = stop - start;

			if ((e & JENTRY_TYPEMASK) != JENTRY_ISSTRING)
				bad_value("a jsonb key that isn't a string");
			/* jsonb's order: shorter first, then by bytes */
			if (i > 0 &&
				(len < key_len ||
				 (len == key_len &&
				  memcmp(doc + data + key_start, doc + from, len) >= 0)))
				bad_value("jsonb keys out of order, or a key twice");
			key_start = start;
			key_len = len;
		}
		switch (e & JENTRY_TYPEMASK)
		{
			case JENTRY_ISSTRING:
				if (!pg_verify_mbstr(PG_UTF8, doc + from, stop - start, true))
					bad_value("a jsonb string that isn't UTF-8");
				break;
			case JENTRY_ISNUMERIC:
				{
					uint32		pad = jsonb_padding(doc, from, data + stop);
					const char *num = doc + from + pad;
					uint32		len = stop - start - pad;

					if (len < VARHDRSZ || !VARATT_IS_4B_U(num) ||
						VARSIZE(num) != len ||
						!numeric_is_canonical((Numeric) unconstify(char *, num)))
						bad_value("a jsonb numeric that isn't one numeric builds");
					break;
				}
			case JENTRY_ISBOOL_FALSE:
			case JENTRY_ISBOOL_TRUE:
			case JENTRY_ISNULL:
				if (stop != start)
					bad_value("a jsonb scalar with bytes");
				break;
			case JENTRY_ISCONTAINER:
				if ((header & JB_FSCALAR) != 0)
					bad_value("a jsonb container as a scalar");
				check_jsonb_container(doc,
									  from + jsonb_padding(doc, from, data + stop),
									  data + stop, false);
				break;
			default:
				bad_value("a jsonb entry of no kind jsonb has");
		}
		prev = stop;
	}
	if (data + prev != end)
		bad_value("bytes after a jsonb container's last child");
}

static Datum
decode_jsonb(ProllyDatumCodec *c, const char *data, int len)
{
	Jsonb	   *jb;

	if (len >= 1 && (uint8) data[0] == JSONB_SEND_VERSION)
		return recv_utf8(c, data, len);
	if (len < 1 || (uint8) data[0] != JSONB_CANONICAL_VERSION)
		bad_value("a jsonb version this server doesn't know");
	jb = palloc(VARHDRSZ + len - 1);
	SET_VARSIZE(jb, VARHDRSZ + len - 1);
	memcpy(VARDATA(jb), data + 1, len - 1);
	check_jsonb_container(VARDATA(jb), 0, len - 1, true);
	if (GetDatabaseEncoding() != PG_UTF8 && !all_ascii(data + 1, len - 1))
	{
		Jsonb	   *s = jsonb_convert(jb, false);

		pfree(jb);
		jb = s;
	}
	return JsonbPGetDatum(jb);
}

/* ------------------------------------------------------------------------
 * Encode and decode
 * ------------------------------------------------------------------------
 */

void
prolly_datum_encode(ProllyDatumCodec *c, Datum value, StringInfo out)
{
	switch (c->kind)
	{
		case CODEC_INT2:
			put_be(out, (uint16) DatumGetInt16(value), 2);
			break;
		case CODEC_INT4:
			put_be(out, (uint32) DatumGetInt32(value), 4);
			break;
		case CODEC_INT8:
			put_be(out, (uint64) DatumGetInt64(value), 8);
			break;
		case CODEC_BOOL:
			appendStringInfoChar(out, DatumGetBool(value) ? 1 : 0);
			break;
		case CODEC_TEXT:
			{
				text	   *t = DatumGetTextPP(value);

				appendBinaryStringInfo(out, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
				if ((Pointer) t != DatumGetPointer(value))
					pfree(t);
				break;
			}
		case CODEC_ARRAY:
			encode_array(&c->elem, value, out);
			break;
		case CODEC_COMPOSITE:
			encode_composite(c, value, out);
			break;
		case CODEC_ANYARRAY:
			encode_anyarray(c, value, out);
			break;
		case CODEC_JSONB:
			encode_jsonb(value, out);
			break;
		case CODEC_SEND:
			{
				bytea	   *b = send_utf8(&c->send, value);

				appendBinaryStringInfo(out, VARDATA(b), VARSIZE(b) - VARHDRSZ);
				pfree(b);
				break;
			}
	}
}

Datum
prolly_datum_decode(ProllyDatumCodec *c, const char *data, int len)
{
	const char *p = data;
	const char *end = data + len;
	Datum		result;

	switch (c->kind)
	{
		case CODEC_INT2:
			if (len != 2)
				bad_value("int2 is not 2 bytes");
			return Int16GetDatum((int16) get_be(data, 2));
		case CODEC_INT4:
			if (len != 4)
				bad_value("int4 is not 4 bytes");
			return Int32GetDatum((int32) get_be(data, 4));
		case CODEC_INT8:
			if (len != 8)
				bad_value("int8 is not 8 bytes");
			return Int64GetDatum((int64) get_be(data, 8));
		case CODEC_BOOL:
			if (len != 1)
				bad_value("bool is not 1 byte");
			return BoolGetDatum(data[0] != 0);
		case CODEC_TEXT:
			return PointerGetDatum(cstring_to_text_with_len(data, len));
		case CODEC_ARRAY:
			result = decode_array(&c->elem, &p, end);
			break;
		case CODEC_COMPOSITE:
			result = decode_composite(c, &p, end);
			break;
		case CODEC_ANYARRAY:
			result = decode_anyarray(c, &p, end);
			break;
		case CODEC_JSONB:
			return decode_jsonb(c, data, len);
		case CODEC_SEND:
			return recv_utf8(c, data, len);
		default:
			elog(ERROR, "unrecognized prolly datum codec kind %d", c->kind);
			return (Datum) 0;	/* keep compiler quiet */
	}
	if (p != end)
		bad_value("trailing bytes");
	return result;
}
