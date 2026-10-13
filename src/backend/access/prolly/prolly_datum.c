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
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
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
