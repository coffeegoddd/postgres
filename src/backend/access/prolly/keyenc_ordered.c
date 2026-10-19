/*-------------------------------------------------------------------------
 *
 * keyenc_ordered.c
 *	  Key encoder implementation "ordered".
 *
 * Each key column is encoded as a marker byte followed by the value:
 *
 *	marker	0x00 NULL (NULLS FIRST), 0x01 value, 0x02 NULL (NULLS LAST)
 *
 * The value's encoding depends on the column's comparison function (btree
 * support function 1 of its operator family, which prolly's own operator
 * families share):
 *
 *	btint2cmp, btint4cmp, btint8cmp	big-endian, sign bit flipped
 *	btboolcmp						one byte, 0 or 1
 *	bttextcmp, bttext_pattern_cmp	the text in UTF-8, then a 0x00
 *									terminator (text never contains NUL)
 *	anything else					4-byte big-endian length, then the
 *									value's canonical bytes (prolly_datum.h)
 *
 * For DESC columns, the bytes of the first three kinds are inverted
 * (marker excluded), which reverses their order.
 *
 * The integer and bool encodings, and in a UTF-8 database the pattern-text
 * encoding and text under a C collation, sort with memcmp (UTF-8 byte order
 * is code point order), so when every column is one of those,
 * compare() is a single memcmp.  Otherwise compare() walks the columns,
 * comparing memcmp-ordered columns by their bytes and decoding the others to
 * call their comparison function with the column's collation.  That
 * fallback is slower; it is what keeps every btree opclass and collation
 * usable as a prolly key, ordered exactly as btree orders it.
 *
 * The encodings use fixed byte order, UTF-8 text and canonical value
 * bytes, so they are the same on every platform, in every cluster and in
 * every session, and hold no local OIDs.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/keyenc_ordered.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"
#include "varatt.h"

#define MARKER_NULL_FIRST	0x00
#define MARKER_VALUE		0x01
#define MARKER_NULL_LAST	0x02

typedef enum ColumnKind
{
	KIND_INT2,
	KIND_INT4,
	KIND_INT8,
	KIND_BOOL,
	KIND_TEXT,
	KIND_SEND,
} ColumnKind;

typedef struct KeyColumn
{
	ColumnKind	kind;
	bool		memcmp_ordered; /* bytes sort like the comparison function */
	bool		desc;
	bool		nulls_first;
	bool		byval;			/* decoded values are not palloc'd */
	Oid			collation;
	FmgrInfo	cmp;			/* if !memcmp_ordered */
	ProllyDatumCodec *codec;	/* KIND_SEND */
} KeyColumn;

struct ProllyKeyEncoder
{
	int			ncols;
	bool		all_memcmp;
	KeyColumn  *cols;
};

/* A column's encoded bytes within a key, after its marker. */
typedef struct Segment
{
	uint8		marker;
	const uint8 *data;			/* value bytes, terminator included */
	uint32		len;
} Segment;

/*
 * The kind of column for (opfamily, type, collation), and whether its bytes
 * sort with memcmp.  Returns false if the column can't be encoded.
 */
/*
 * The opfamily's comparison function for type.  Families for arrays, enums,
 * ranges, multiranges and composites register theirs for the polymorphic
 * type, not for each concrete one.
 */
static Oid
comparison_proc(Oid opfamily, Oid base)
{
	Oid			proc;
	Oid			poly = InvalidOid;

	/*
	 * Only an ordering access method's support function 1 compares (btree's,
	 * or prolly's): GiST's, for one, is its consistent function.
	 */
	if (!GetIndexAmRoutineByAmId(get_opfamily_method(opfamily), false)->amcanorder)
		return InvalidOid;
	proc = get_opfamily_proc(opfamily, base, base, BTORDER_PROC);

	if (OidIsValid(proc))
		return proc;
	if (OidIsValid(get_element_type(base)))
		poly = ANYARRAYOID;
	else
		switch (get_typtype(base))
		{
			case TYPTYPE_ENUM:
				poly = ANYENUMOID;
				break;
			case TYPTYPE_RANGE:
				poly = ANYRANGEOID;
				break;
			case TYPTYPE_MULTIRANGE:
				poly = ANYMULTIRANGEOID;
				break;
			case TYPTYPE_COMPOSITE:
				poly = RECORDOID;
				break;
		}
	return OidIsValid(poly) ?
		get_opfamily_proc(opfamily, poly, poly, BTORDER_PROC) : InvalidOid;
}

static bool
classify(Oid opfamily, Oid type, Oid collation, ColumnKind *kind,
		 bool *memcmp_ordered, Oid *cmpproc)
{
	Oid			base = getBaseType(type);
	Oid			proc = comparison_proc(opfamily, base);

	if (!OidIsValid(proc))
		return false;
	*cmpproc = proc;
	*memcmp_ordered = true;

	if (proc == F_BTINT2CMP && base == INT2OID)
		*kind = KIND_INT2;
	else if (proc == F_BTINT4CMP && base == INT4OID)
		*kind = KIND_INT4;
	else if (proc == F_BTINT8CMP && base == INT8OID)
		*kind = KIND_INT8;
	else if (proc == F_BTBOOLCMP && base == BOOLOID)
		*kind = KIND_BOOL;
	else if (proc == F_BTTEXT_PATTERN_CMP && (base == TEXTOID || base == VARCHAROID))
	{
		*kind = KIND_TEXT;
		*memcmp_ordered = GetDatabaseEncoding() == PG_UTF8;
	}
	else if (proc == F_BTTEXTCMP && (base == TEXTOID || base == VARCHAROID))
	{
		if (!OidIsValid(collation))
			return false;
		*kind = KIND_TEXT;
		*memcmp_ordered = GetDatabaseEncoding() == PG_UTF8 &&
			pg_newlocale_from_collation(collation)->collate_is_c;
	}
	else
	{
		/* Any other type, if it has a binary send and receive format. */
		HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(base));
		bool		binary_io;

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", base);
		binary_io = OidIsValid(((Form_pg_type) GETSTRUCT(tup))->typsend) &&
			OidIsValid(((Form_pg_type) GETSTRUCT(tup))->typreceive);
		ReleaseSysCache(tup);
		if (!binary_io)
			return false;
		*kind = KIND_SEND;
		*memcmp_ordered = false;
	}
	return true;
}

static bool
ordered_supports(Oid opfamily, Oid type, Oid collation)
{
	ColumnKind	kind;
	bool		memcmp_ordered;
	Oid			cmpproc;

	return classify(opfamily, type, collation, &kind, &memcmp_ordered, &cmpproc);
}

static ProllyKeyEncoder *
ordered_begin(const ProllyKeyDesc *desc, MemoryContext mcxt)
{
	ProllyKeyEncoder *enc = MemoryContextAlloc(mcxt, sizeof(ProllyKeyEncoder));

	enc->ncols = desc->nkeys;
	enc->cols = MemoryContextAllocZero(mcxt, sizeof(KeyColumn) * desc->nkeys);
	enc->all_memcmp = true;

	for (int i = 0; i < desc->nkeys; i++)
	{
		KeyColumn  *col = &enc->cols[i];
		Oid			base = getBaseType(desc->types[i]);
		Oid			cmpproc;

		if (!classify(desc->opfamilies[i], desc->types[i], desc->collations[i],
					  &col->kind, &col->memcmp_ordered, &cmpproc))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prolly key encoder cannot encode type %s with operator family %u",
							format_type_be(desc->types[i]), desc->opfamilies[i])));

		col->desc = (desc->options[i] & INDOPTION_DESC) != 0;
		col->nulls_first = (desc->options[i] & INDOPTION_NULLS_FIRST) != 0;
		col->collation = desc->collations[i];
		col->byval = col->kind != KIND_TEXT &&
			(col->kind != KIND_SEND || get_typbyval(base));
		if (!col->memcmp_ordered)
		{
			enc->all_memcmp = false;
			fmgr_info_cxt(cmpproc, &col->cmp, mcxt);
		}
		if (col->kind == KIND_SEND)
			col->codec = prolly_datum_codec(desc->types[i], -1, mcxt);
	}
	return enc;
}

static void
append_be(StringInfo out, uint64 v, int nbytes)
{
	uint8		buf[8];

	for (int i = nbytes - 1; i >= 0; i--)
	{
		buf[i] = (uint8) (v & 0xff);
		v >>= 8;
	}
	appendBinaryStringInfo(out, buf, nbytes);
}

static uint64
read_be(const uint8 *p, int nbytes)
{
	uint64		v = 0;

	for (int i = 0; i < nbytes; i++)
		v = (v << 8) | p[i];
	return v;
}

static void
ordered_encode(ProllyKeyEncoder *enc, const Datum *values, const bool *isnull,
			   StringInfo out)
{
	for (int i = 0; i < enc->ncols; i++)
	{
		KeyColumn  *col = &enc->cols[i];
		int			start;

		if (isnull[i])
		{
			appendStringInfoChar(out, col->nulls_first ? MARKER_NULL_FIRST
								 : MARKER_NULL_LAST);
			continue;
		}
		appendStringInfoChar(out, MARKER_VALUE);
		start = out->len;

		switch (col->kind)
		{
			case KIND_INT2:
				append_be(out, (uint16) DatumGetInt16(values[i]) ^ 0x8000, 2);
				break;
			case KIND_INT4:
				append_be(out, (uint32) DatumGetInt32(values[i]) ^ 0x80000000, 4);
				break;
			case KIND_INT8:
				append_be(out, (uint64) DatumGetInt64(values[i]) ^
						  UINT64CONST(0x8000000000000000), 8);
				break;
			case KIND_BOOL:
				appendStringInfoChar(out, DatumGetBool(values[i]) ? 1 : 0);
				break;
			case KIND_TEXT:
				{
					text	   *t = DatumGetTextPP(values[i]);
					int			len = VARSIZE_ANY_EXHDR(t);
					char	   *utf8 = pg_server_to_any(VARDATA_ANY(t), len, PG_UTF8);

					if (utf8 != VARDATA_ANY(t))
						len = strlen(utf8);
					appendBinaryStringInfo(out, utf8, len);
					appendStringInfoChar(out, '\0');
					if ((Pointer) t != DatumGetPointer(values[i]))
						pfree(t);
					break;
				}
			case KIND_SEND:
				{
					StringInfoData b;

					initStringInfo(&b);
					prolly_datum_encode(col->codec, values[i], &b);
					append_be(out, b.len, 4);
					appendBinaryStringInfo(out, b.data, b.len);
					pfree(b.data);
					break;
				}
		}

		if (col->desc && col->kind != KIND_SEND)
		{
			for (int j = start; j < out->len; j++)
				out->data[j] = ~out->data[j];
		}
	}
}

/* Parse column col's segment at *p, advancing *p. */
static void
next_segment(const KeyColumn *col, const uint8 **p, const uint8 *end,
			 Segment *seg)
{
	const uint8 *q = *p;
	uint32		len;

	if (q >= end)
		elog(ERROR, "prolly key is truncated");
	seg->marker = *q++;
	seg->data = q;
	if (seg->marker != MARKER_VALUE)
	{
		if (seg->marker != MARKER_NULL_FIRST && seg->marker != MARKER_NULL_LAST)
			elog(ERROR, "prolly key has bad marker byte 0x%02x", seg->marker);
		seg->len = 0;
		*p = q;
		return;
	}

	switch (col->kind)
	{
		case KIND_INT2:
			len = 2;
			break;
		case KIND_INT4:
			len = 4;
			break;
		case KIND_INT8:
			len = 8;
			break;
		case KIND_BOOL:
			len = 1;
			break;
		case KIND_TEXT:
			{
				uint8		term = col->desc ? 0xff : 0x00;
				const uint8 *t = memchr(q, term, end - q);

				if (t == NULL)
					elog(ERROR, "prolly key text is not terminated");
				len = t - q + 1;
				break;
			}
		case KIND_SEND:
			if (end - q < 4)
				elog(ERROR, "prolly key is truncated");
			len = 4 + (uint32) read_be(q, 4);
			break;
		default:
			elog(ERROR, "unrecognized prolly key column kind %d", col->kind);
			len = 0;			/* keep compiler quiet */
	}
	if (end - q < len)
		elog(ERROR, "prolly key is truncated");
	seg->len = len;
	*p = q + len;
}

/* The value of a non-NULL segment.  The result is palloc'd if by reference. */
static Datum
segment_value(const KeyColumn *col, const Segment *seg)
{
	uint8		buf[8];
	const uint8 *b = seg->data;

	if (col->desc && col->kind != KIND_SEND && col->kind != KIND_TEXT)
	{
		for (uint32 i = 0; i < seg->len; i++)
			buf[i] = ~seg->data[i];
		b = buf;
	}

	switch (col->kind)
	{
		case KIND_INT2:
			return Int16GetDatum((int16) (read_be(b, 2) ^ 0x8000));
		case KIND_INT4:
			return Int32GetDatum((int32) (read_be(b, 4) ^ 0x80000000));
		case KIND_INT8:
			return Int64GetDatum((int64) (read_be(b, 8) ^
										  UINT64CONST(0x8000000000000000)));
		case KIND_BOOL:
			return BoolGetDatum(b[0] != 0);
		case KIND_TEXT:
			{
				uint32		n = seg->len - 1;	/* without terminator */
				char	   *utf8 = palloc(n + 1);
				char	   *server;
				Datum		d;

				for (uint32 i = 0; i < n; i++)
					utf8[i] = col->desc ? ~seg->data[i] : seg->data[i];
				utf8[n] = '\0';
				server = pg_any_to_server(utf8, n, PG_UTF8);
				d = PointerGetDatum(cstring_to_text_with_len(server,
															 server == utf8 ? n : strlen(server)));
				pfree(utf8);
				return d;
			}
		case KIND_SEND:
			{
				return prolly_datum_decode(col->codec, (const char *) seg->data + 4,
										   seg->len - 4);
			}
	}
	elog(ERROR, "unrecognized prolly key column kind %d", col->kind);
	return (Datum) 0;			/* keep compiler quiet */
}

static int
compare_bytes(const uint8 *a, uint32 alen, const uint8 *b, uint32 blen)
{
	int			r = memcmp(a, b, Min(alen, blen));

	if (r != 0)
		return r < 0 ? -1 : 1;
	return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static int
ordered_compare(ProllyKeyEncoder *enc, const ProllyKey *a, const ProllyKey *b)
{
	const uint8 *pa = a->data;
	const uint8 *pb = b->data;
	const uint8 *enda = a->data + a->len;
	const uint8 *endb = b->data + b->len;

	if (enc->all_memcmp)
		return compare_bytes(a->data, a->len, b->data, b->len);

	for (int i = 0; i < enc->ncols; i++)
	{
		KeyColumn  *col = &enc->cols[i];
		Segment		sa;
		Segment		sb;
		int			r;

		next_segment(col, &pa, enda, &sa);
		next_segment(col, &pb, endb, &sb);

		if (sa.marker != sb.marker)
			return sa.marker < sb.marker ? -1 : 1;
		if (sa.marker != MARKER_VALUE)
			continue;

		if (col->memcmp_ordered)
			r = compare_bytes(sa.data, sa.len, sb.data, sb.len);
		else
		{
			Datum		da = segment_value(col, &sa);
			Datum		db = segment_value(col, &sb);

			r = DatumGetInt32(FunctionCall2Coll(&col->cmp, col->collation, da, db));
			if (col->desc)
				INVERT_COMPARE_RESULT(r);
			if (!col->byval)
			{
				pfree(DatumGetPointer(da));
				pfree(DatumGetPointer(db));
			}
		}
		if (r != 0)
			return r < 0 ? -1 : 1;
	}
	return 0;
}

static void
ordered_end(ProllyKeyEncoder *enc)
{
	pfree(enc->cols);
	pfree(enc);
}

const ProllyKeyEncoderRoutine prolly_keyenc_ordered = {
	.info = {
		.iface = PROLLY_IFACE_KEY_ENCODER,
		.impl_id = 1,
		.format_version = 2,		/* 2: jsonb as stored */
		.name = "ordered",
		.description = "memcmp-ordered integers, bools and C-collation text; "
		"other types by canonical bytes and their comparison function",
	},
	.supports = ordered_supports,
	.begin = ordered_begin,
	.encode = ordered_encode,
	.compare = ordered_compare,
	.end = ordered_end,
};
