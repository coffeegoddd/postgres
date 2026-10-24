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

#include <math.h>

#include "access/htup_details.h"
#include "access/prolly_datum.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/datum.h"
#include "utils/float.h"
#include "utils/geo_decls.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/multirangetypes.h"
#include "utils/numeric.h"
#include "utils/rangetypes.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

typedef enum CodecKind
{
	CODEC_INT2,
	CODEC_INT4,
	CODEC_INT8,
	CODEC_BOOL,
	CODEC_FLOAT4,
	CODEC_FLOAT8,
	CODEC_TEXT,					/* text, varchar, bpchar in a UTF-8 database */
	CODEC_ARRAY,
	CODEC_COMPOSITE,
	CODEC_ANYARRAY,
	CODEC_JSONB,
	CODEC_SEND,
	CODEC_OUTPUT,				/* a type without binary send/receive */
} CodecKind;

/*
 * The tag a value of a type whose binary I/O may change starts with: which
 * form the rest is, so a value is read back as it was written whatever the
 * type has now (an extension's update may give it send and receive, or
 * take them away).
 */
#define TAG_SEND	0			/* its binary send format */
#define TAG_OUTPUT	1			/* its text form */

/*
 * The floats a CODEC_SEND value holds, whose NaNs are made canonical
 * before it's sent.
 */
typedef enum SendFloats
{
	FLOATS_NONE,
	FLOATS_GEOMETRIC,			/* float8s from floats_from to the end */
	FLOATS_RANGE,				/* bounds of type floats_type */
	FLOATS_MULTIRANGE,			/* ranges' bounds of type floats_type */
} SendFloats;

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
	SendFloats	floats;
	int			floats_from;	/* FLOATS_GEOMETRIC: the first float8's offset */
	Oid			floats_type;	/* FLOATS_*RANGE: float4 or float8 */
	TypeCacheEntry *floats_range;	/* FLOATS_*RANGE: the range type's */
	int16		floats_len;		/* FLOATS_GEOMETRIC: typlen */

	/*
	 * CODEC_OUTPUT (ioparam above too); and a tagged CODEC_SEND's, to read
	 * a value written in its text form
	 */
	FmgrInfo	output;
	FmgrInfo	input;
	Oid			input_ioparam;
	int16		typlen;
	bool		typbyval;
	bool		tagged;			/* values start with TAG_SEND or TAG_OUTPUT */
	bool		normalizes;		/* CODEC_SEND: store send(receive(send())) */
	bool		has_recv;		/* CODEC_OUTPUT: a TAG_SEND value is readable */

	/* CODEC_ARRAY */
	ElemInfo	elem;

	/* CODEC_COMPOSITE */
	TupleDesc	tupdesc;
	ProllyDatumCodec **attcodecs;	/* NULL for dropped attributes */

	/* CODEC_ANYARRAY: element codecs built so far */
	List	   *anyarray_elems;	/* of ElemInfo * */

	/* Historical decoding; the mappings outlive this codec. */
	ProllyEnumMapping *enum_mapping;
	List	   *enum_targets;
	List	   *enum_mappings;	/* also for lazily built anyarray codecs */
};

/*
 * The labels a mapping's saved labels read as, or a refusal where they
 * couldn't be worked out (a change between two of the enum's definitions
 * that's neither a rename in place nor an addition keeping the old labels
 * in order: snapshots don't record the ALTER TYPE commands made).
 */
static List *
enum_targets(ProllyEnumMapping *m)
{
	if (m->targets == NIL && m->before != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("schema conflict: cannot map historical labels of enum %s",
						format_type_be(m->type)),
				 errdetail("Labels are read across commits that rename labels in place or add labels keeping the others in order, not across one that does both or reuses a label."),
				 errhint("Commit an enum's renames and its additions separately, and merge row changes before reusing labels.")));
	return m->targets;
}

/* Attach mappings only to decoders for the enums they describe. */
bool
prolly_datum_map_enums(ProllyDatumCodec *c, List *mappings)
{
	bool		changed = false;
	ListCell   *lc;

	c->enum_mappings = mappings;
	c->enum_mapping = NULL;
	c->enum_targets = NIL;
	foreach(lc, mappings)
	{
		ProllyEnumMapping *m = lfirst(lc);

		if (c->type == m->type)
		{
			c->enum_mapping = m;
			c->enum_targets = enum_targets(m);
			changed = true;
		}
	}
	if (c->kind == CODEC_ARRAY)
		changed |= prolly_datum_map_enums(c->elem.codec, mappings);
	else if (c->kind == CODEC_COMPOSITE)
	{
		for (int i = 0; i < c->tupdesc->natts; i++)
			if (c->attcodecs[i])
				changed |= prolly_datum_map_enums(c->attcodecs[i], mappings);
	}
	else if (c->kind == CODEC_ANYARRAY)
	{
		changed = mappings != NIL;
		foreach(lc, c->anyarray_elems)
			(void) prolly_datum_map_enums(((ElemInfo *) lfirst(lc))->codec, mappings);
	}
	return changed;
}

/* Decode only labels that actually belonged to the saved definition. */
static const char *
historical_enum_label(ProllyDatumCodec *c, const char *label)
{
	List	   *before = c->enum_mapping->before;

	for (int i = 0; i < list_length(before); i++)
		if (strcmp(strVal(list_nth(before, i)), label) == 0)
		{
			String	   *target = list_nth(c->enum_targets, i);

			if (target == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("schema conflict: enum label \"%s\" of %s has no label to read as",
								label, format_type_be(c->type)),
						 errdetail("It was added where the enum it's read as doesn't have it.")));
			return strVal(target);
		}
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("enum label \"%s\" is absent from its saved definition", label)));
	return NULL;
}

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
 * Floats
 *
 * A float's canonical bytes are its send format, its IEEE 754 bits in
 * network byte order, except that every NaN is the positive quiet NaN (the
 * bits 'NaN' has on most platforms).  A NaN's sign and payload are bits no
 * operator sees: every NaN equals every other and prints as NaN, but
 * 'NaN' and 'inf' - 'inf' differ in their bits, so without this, equal
 * rows would have different bytes.  (-0 and 0 are equal too, but print
 * differently, so they stay different bytes.)  The floats inside a value
 * sent whole are made so too: geometric values' coordinates, and the
 * bounds of ranges and multiranges of floats.  A NaN of other bits, as
 * bytes written before, decodes as it is.
 * ------------------------------------------------------------------------
 */

#define CANONICAL_FLOAT4_NAN	0x7fc00000
#define CANONICAL_FLOAT8_NAN	UINT64CONST(0x7ff8000000000000)

/* f's canonical bits */
static inline uint32
float4_bits(float4 f)
{
	uint32		bits;

	if (isnan(f))
		return CANONICAL_FLOAT4_NAN;
	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

static inline uint64
float8_bits(float8 f)
{
	uint64		bits;

	if (isnan(f))
		return CANONICAL_FLOAT8_NAN;
	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

/*
 * A float4 or float8 Datum with a NaN made canonical; *changed set when its
 * bits were others.
 */
static Datum
canonical_float(Oid type, Datum value, bool *changed)
{
	if (type == FLOAT4OID)
	{
		float4		f = DatumGetFloat4(value);
		uint32		bits = float4_bits(f);

		if (memcmp(&bits, &f, sizeof(bits)) == 0)
			return value;
		memcpy(&f, &bits, sizeof(f));
		*changed = true;
		return Float4GetDatum(f);
	}
	else
	{
		float8		f = DatumGetFloat8(value);
		uint64		bits = float8_bits(f);

		if (memcmp(&bits, &f, sizeof(bits)) == 0)
			return value;
		memcpy(&f, &bits, sizeof(f));
		*changed = true;
		return Float8GetDatum(f);
	}
}

/* Whether a range bound is a NaN of other bits than the canonical NaN's */
static bool
bound_noncanonical(Oid type, const RangeBound *b)
{
	bool		changed = false;

	if (!b->infinite)
		(void) canonical_float(type, b->val, &changed);
	return changed;
}

/* r, or a copy with its bounds' NaNs made canonical */
static RangeType *
canonical_range(TypeCacheEntry *typcache, RangeType *r, Oid type)
{
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	bool		changed = false;

	range_deserialize(typcache, r, &lower, &upper, &empty);
	if (empty)
		return r;
	if (!lower.infinite)
		lower.val = canonical_float(type, lower.val, &changed);
	if (!upper.infinite)
		upper.val = canonical_float(type, upper.val, &changed);
	return changed ? range_serialize(typcache, &lower, &upper, false, NULL) : r;
}

/*
 * A CODEC_SEND value, or a copy of it with the NaNs it holds made
 * canonical, for its send function.
 */
static Datum
send_floats(ProllyDatumCodec *c, Datum value)
{
	switch (c->floats)
	{
		case FLOATS_NONE:
			break;
		case FLOATS_GEOMETRIC:
			{
				/* nothing but float8s from floats_from on */
				char	   *data;
				Size		size;
				char	   *copy = NULL;

				if (c->floats_len == -1)
				{
					data = (char *) PG_DETOAST_DATUM(value);
					size = VARSIZE(data);
				}
				else
				{
					data = DatumGetPointer(value);
					size = c->floats_len;
				}
				for (Size at = c->floats_from; at + sizeof(float8) <= size;
					 at += sizeof(float8))
				{
					float8		f;
					uint64		bits;

					memcpy(&f, data + at, sizeof(f));
					bits = float8_bits(f);
					if (memcmp(&bits, &f, sizeof(bits)) == 0)
						continue;
					if (copy == NULL)
					{
						copy = palloc(size);
						memcpy(copy, data, size);
					}
					memcpy(copy + at, &bits, sizeof(bits));
				}
				if (copy != NULL)
					return PointerGetDatum(copy);
				break;
			}
		case FLOATS_RANGE:
			{
				RangeType  *r = DatumGetRangeTypeP(value);
				RangeType  *canon = canonical_range(c->floats_range, r,
													c->floats_type);

				if (canon != r)
					return RangeTypePGetDatum(canon);
				break;
			}
		case FLOATS_MULTIRANGE:
			{
				TypeCacheEntry *rngtyp = c->floats_range;
				MultirangeType *mr = DatumGetMultirangeTypeP(value);
				int32		n;
				RangeType **ranges;
				bool		changed = false;

				/*
				 * Its bounds read in place first: a NaN of other bits is
				 * rare, and only then are its ranges copied out and rebuilt.
				 */
				for (uint32 i = 0; i < mr->rangeCount && !changed; i++)
				{
					RangeBound	lower;
					RangeBound	upper;

					multirange_get_bounds(rngtyp, mr, i, &lower, &upper);
					changed = bound_noncanonical(c->floats_type, &lower) ||
						bound_noncanonical(c->floats_type, &upper);
				}
				if (!changed)
					break;
				changed = false;
				multirange_deserialize(rngtyp, mr, &n, &ranges);
				for (int i = 0; i < n; i++)
				{
					RangeType  *canon = canonical_range(rngtyp, ranges[i],
													   c->floats_type);

					changed |= canon != ranges[i];
					ranges[i] = canon;
				}
				if (changed)
					return MultirangeTypePGetDatum(make_multirange(c->type, rngtyp,
																   n, ranges));
				break;
			}
	}
	return value;
}

/* ------------------------------------------------------------------------
 * Send and receive with the client encoding fixed to UTF-8
 * ------------------------------------------------------------------------
 */

/*
 * Settings a type's text form may depend on, fixed while one is written or
 * read: the shortest exact float output (extra_float_digits 1), ISO dates
 * in MDY order, the postgres interval style.  A nest level to end, or -1 if
 * they were so already.
 */
static int
fixed_text_settings(void)
{
	int			nest;

	if (extra_float_digits == 1 && DateStyle == USE_ISO_DATES &&
		DateOrder == DATEORDER_MDY && IntervalStyle == INTSTYLE_POSTGRES)
		return -1;
	nest = NewGUCNestLevel();
	(void) set_config_option("extra_float_digits", "1", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("datestyle", "ISO, MDY", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("intervalstyle", "postgres", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	return nest;
}

static void
end_text_settings(int nest)
{
	if (nest >= 0)
		AtEOXact_GUC(true, nest);
}

/*
 * A CODEC_OUTPUT value: TAG_OUTPUT, then its text form in UTF-8, written
 * with fixed settings.  The text must read back as the value, byte for
 * byte (datum_image_eq), or the value isn't stored: a type whose text form
 * drops precision (PostGIS's box2d, at 15 significant digits) can't be
 * kept this way, and it is refused where it would be written rather than
 * changed.
 */
static void
encode_output(ProllyDatumCodec *c, Datum value, StringInfo out)
{
	int			nest = fixed_text_settings();
	char	   *text = OutputFunctionCall(&c->output, value);
	Datum		back = InputFunctionCall(&c->input, text, c->ioparam, c->typmod);
	char	   *utf8;

	end_text_settings(nest);
	if (!datum_image_eq(value, back, c->typbyval, c->typlen))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("value of type %s can't be stored in a versioned table: its text form doesn't read back as the same value",
						format_type_be(c->type)),
				 errdetail("The value is \"%s\".", text),
				 errhint("The type has no binary send and receive functions, so a versioned table keeps its values as text.")));
	utf8 = pg_server_to_any(text, strlen(text), PG_UTF8);
	appendStringInfoChar(out, TAG_OUTPUT);
	appendBinaryStringInfo(out, utf8, strlen(utf8));
	if (utf8 != text)
		pfree(utf8);
	pfree(text);
}

/* a value's text form (after its tag), read with fixed settings */
static Datum
decode_text(ProllyDatumCodec *c, const char *data, int len)
{
	char	   *text = pnstrdup(data, len);
	char	   *server;
	int			nest;
	Datum		result;

	if (strlen(text) != len)
		bad_value("text form contains a zero byte");
	server = pg_any_to_server(text, len, PG_UTF8);
	nest = fixed_text_settings();
	result = InputFunctionCall(&c->input, server, c->input_ioparam, c->typmod);
	end_text_settings(nest);
	return result;
}

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

/* a type's receive function, or InvalidOid */
static Oid
get_type_receive(Oid type)
{
	HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	Oid			recv;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type);
	recv = ((Form_pg_type) GETSTRUCT(tup))->typreceive;
	ReleaseSysCache(tup);
	return recv;
}

/* a range's or multirange's subtype (domains looked through), or InvalidOid */
static Oid
range_subtype(Oid base)
{
	if (type_is_range(base))
		return getBaseType(get_range_subtype(base));
	if (type_is_multirange(base))
		return getBaseType(get_range_subtype(get_multirange_range(base)));
	return InvalidOid;
}

/*
 * Has base type (domains looked through) no binary send or receive, or is it
 * a range over one?  (Arrays and composites are their elements'.)
 */
static bool
lacks_binary_io(Oid base)
{
	HeapTuple	tup;
	Form_pg_type typ;
	bool		lacks;
	Oid			sub = range_subtype(base);

	if (OidIsValid(sub))
		return lacks_binary_io(sub);
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(base));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", base);
	typ = (Form_pg_type) GETSTRUCT(tup);
	lacks = typ->typtype == TYPTYPE_BASE &&
		(!OidIsValid(typ->typsend) || !OidIsValid(typ->typreceive));
	ReleaseSysCache(tup);
	return lacks;
}

/*
 * May base type's binary I/O change: a base type initdb didn't make (an
 * extension's, whose update may give it send and receive or take them
 * away), or a range over one?  initdb's types' I/O is fixed.
 */
static bool
io_may_change(Oid base)
{
	Oid			sub = range_subtype(base);

	if (OidIsValid(sub))
		return io_may_change(sub);
	return base >= FirstNormalObjectId && get_typtype(base) == TYPTYPE_BASE;
}

/*
 * May binary receive of base type (domains looked through) normalize a
 * value: an extension base type's receive, reached directly or through a
 * range's subtype, an array's elements or a composite's attributes.  A range
 * over a composite holding an extension type is untagged, but range_recv
 * still calls that type's receive for its bounds.
 */
static bool
io_may_normalize(Oid base)
{
	Oid			sub = range_subtype(base);
	Oid			elem;

	if (OidIsValid(sub))
		return io_may_normalize(sub);
	elem = get_element_type(base);
	if (OidIsValid(elem))
		return io_may_normalize(getBaseType(elem));
	if (get_typtype(base) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(base, -1);
		bool		normalizes = false;

		for (int i = 0; i < desc->natts && !normalizes; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!att->attisdropped)
				normalizes = io_may_normalize(getBaseType(att->atttypid));
		}
		ReleaseTupleDesc(desc);
		return normalizes;
	}
	return io_may_change(base);
}

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
		case FLOAT4OID:
			c->kind = CODEC_FLOAT4;
			return c;
		case FLOAT8OID:
			c->kind = CODEC_FLOAT8;
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

	/*
	 * A type without binary send or receive (an extension's, say), or a
	 * range over one, is stored as its text form, which encoding checks
	 * reads back as the value it came from.  A type whose binary I/O may
	 * change (one initdb didn't make, or a range over one) has its values
	 * tagged with the form they're in.
	 */
	{
		Oid			input;
		bool		varlena;

		get_typlenbyval(base, &c->typlen, &c->typbyval);
		c->tagged = io_may_change(base);
		getTypeInputInfo(base, &input, &c->input_ioparam);
		fmgr_info_cxt(input, &c->input, mcxt);
		if (lacks_binary_io(base))
		{
			Oid			output;
			Oid			recv;

			c->kind = CODEC_OUTPUT;
			c->tagged = true;
			c->ioparam = c->input_ioparam;
			getTypeOutputInfo(base, &output, &varlena);
			fmgr_info_cxt(output, &c->output, mcxt);
			recv = get_type_receive(base);
			if (OidIsValid(recv))
			{
				Oid			ioparam;

				getTypeBinaryInputInfo(base, &recv, &ioparam);
				fmgr_info_cxt(recv, &c->recv, mcxt);
				c->has_recv = true;
			}
			return c;
		}
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
		c->normalizes = io_may_normalize(base);
	}

	/*
	 * the floats it holds: for the geometric types, floats_from is where the
	 * float8s the type's send function sends begin in the value (it sends
	 * them all from there on, and before them no floats: a count, a flag)
	 */
	switch (base)
	{
		case POINTOID:
		case LSEGOID:
		case BOXOID:
		case LINEOID:
		case CIRCLEOID:
			c->floats = FLOATS_GEOMETRIC;
			c->floats_from = 0;
			break;
		case PATHOID:
			c->floats = FLOATS_GEOMETRIC;
			c->floats_from = offsetof(PATH, p);
			break;
		case POLYGONOID:
			c->floats = FLOATS_GEOMETRIC;
			c->floats_from = offsetof(POLYGON, p);	/* not boundbox: unsent */
			break;
		default:
			{
				Oid			sub = InvalidOid;

				if (type_is_range(base))
				{
					c->floats = FLOATS_RANGE;
					sub = getBaseType(get_range_subtype(base));
				}
				else if (type_is_multirange(base))
				{
					c->floats = FLOATS_MULTIRANGE;
					sub = getBaseType(get_range_subtype(get_multirange_range(base)));
				}
				if (sub == FLOAT4OID || sub == FLOAT8OID)
				{
					c->floats_type = sub;
					c->floats_range = c->floats == FLOATS_RANGE ?
						lookup_type_cache(base, TYPECACHE_RANGE_INFO) :
						lookup_type_cache(base, TYPECACHE_MULTIRANGE_INFO)->rngtype;
				}
				else
					c->floats = FLOATS_NONE;
			}
	}
	if (c->floats == FLOATS_GEOMETRIC)
		c->floats_len = get_typlen(base);
	return c;
}

void
prolly_datum_definition(Oid typid, StringInfo out)
{
	Oid			base = getBaseType(typid);
	Oid			elemtype;

	check_stack_depth();
	appendBinaryStringInfo(out, (const char *) &base, sizeof(base));
	elemtype = get_element_type(base);
	if (OidIsValid(elemtype))
	{
		prolly_datum_definition(elemtype, out);
		return;
	}
	switch (get_typtype(base))
	{
		case TYPTYPE_ENUM:
			{
				/* (each label by its OID, which rows hold: renamed, sent otherwise) */
				CatCList   *labels = SearchSysCacheList1(ENUMTYPOIDNAME,
														 ObjectIdGetDatum(base));

				prolly_put_varint(out, labels->n_members);
				for (int i = 0; i < labels->n_members; i++)
				{
					Form_pg_enum e = (Form_pg_enum) GETSTRUCT(&labels->members[i]->tuple);
					const char *label = NameStr(e->enumlabel);

					appendBinaryStringInfo(out, (const char *) &e->oid, sizeof(e->oid));
					prolly_put_varint(out, strlen(label));
					appendStringInfoString(out, label);
				}
				ReleaseSysCacheList(labels);
			}
			break;
		case TYPTYPE_COMPOSITE:
			{
				/* (its attributes by number, dropped ones as such) */
				TupleDesc	desc = lookup_rowtype_tupdesc(base, -1);

				prolly_put_varint(out, desc->natts);
				for (int i = 0; i < desc->natts; i++)
				{
					Form_pg_attribute att = TupleDescAttr(desc, i);

					appendStringInfoChar(out, att->attisdropped ? 'd' : 'a');
					if (!att->attisdropped)
						prolly_datum_definition(att->atttypid, out);
				}
				ReleaseTupleDesc(desc);
			}
			break;
		case TYPTYPE_RANGE:
			prolly_datum_definition(get_range_subtype(base), out);
			break;
		case TYPTYPE_MULTIRANGE:
			prolly_datum_definition(get_multirange_range(base), out);
			break;
	}
}

/* An enum's labels in a definition: a count, then each OID and label. */
typedef struct DefLabel
{
	Oid			oid;
	const char *label;
	uint64		len;
} DefLabel;

static DefLabel *
definition_labels(const char **p, const char *end, uint64 *n)
{
	DefLabel   *labels;

	*n = prolly_get_varint(p, end);
	if (*n > (uint64) (end - *p))
		return NULL;
	labels = palloc_array(DefLabel, Max(*n, 1));
	for (uint64 i = 0; i < *n; i++)
	{
		if (end - *p < (ptrdiff_t) sizeof(Oid))
			return NULL;
		memcpy(&labels[i].oid, *p, sizeof(Oid));
		*p += sizeof(Oid);
		labels[i].len = prolly_get_varint(p, end);
		if (labels[i].len > (uint64) (end - *p))
			return NULL;
		labels[i].label = *p;
		*p += labels[i].len;
	}
	return labels;
}

/* (old and now walked together: see prolly_datum_definition_extends()) */
static bool
definition_extends(const char **o, const char *oend, const char **n,
				   const char *nend)
{
	Oid			base;
	Oid			nbase;
	Oid			elemtype;

	check_stack_depth();
	if (oend - *o < (ptrdiff_t) sizeof(Oid) || nend - *n < (ptrdiff_t) sizeof(Oid))
		return false;
	memcpy(&base, *o, sizeof(Oid));
	memcpy(&nbase, *n, sizeof(Oid));
	*o += sizeof(Oid);
	*n += sizeof(Oid);
	if (base != nbase)
		return false;
	elemtype = get_element_type(base);
	if (OidIsValid(elemtype))
		return definition_extends(o, oend, n, nend);
	switch (get_typtype(base))
	{
		case TYPTYPE_ENUM:
			{
				/* (each old label kept, by its OID and name: rows send names) */
				uint64		nold;
				uint64		nnow;
				DefLabel   *old = definition_labels(o, oend, &nold);
				DefLabel   *now = old ? definition_labels(n, nend, &nnow) : NULL;

				if (now == NULL)
					return false;
				for (uint64 i = 0; i < nold; i++)
				{
					bool		kept = false;

					for (uint64 j = 0; j < nnow && !kept; j++)
						kept = now[j].oid == old[i].oid &&
							now[j].len == old[i].len &&
							memcmp(now[j].label, old[i].label, old[i].len) == 0;
					if (!kept)
						return false;
				}
				pfree(old);
				pfree(now);
				return true;
			}
		case TYPTYPE_COMPOSITE:
			{
				/* (the same attributes, each extending its own) */
				uint64		natts = prolly_get_varint(o, oend);

				if (prolly_get_varint(n, nend) != natts)
					return false;
				for (uint64 i = 0; i < natts; i++)
				{
					bool		live;

					if (*o >= oend || *n >= nend || **o != **n)
						return false;
					live = *(*o)++ == 'a';
					(*n)++;
					if (live && !definition_extends(o, oend, n, nend))
						return false;
				}
				return true;
			}
		case TYPTYPE_RANGE:
		case TYPTYPE_MULTIRANGE:
			return definition_extends(o, oend, n, nend);
	}
	return true;
}

bool
prolly_datum_definition_extends(const char *old, int oldlen, const char *now,
								int nowlen)
{
	const char *o = old;
	const char *n = now;

	if (oldlen == nowlen && memcmp(old, now, oldlen) == 0)
		return true;
	return definition_extends(&o, old + oldlen, &n, now + nowlen) &&
		o == old + oldlen && n == now + nowlen;
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
	(void) prolly_datum_map_enums(ei->codec, c->enum_mappings);
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

bool
prolly_datum_codec_normalizes(const ProllyDatumCodec *c)
{
	switch (c->kind)
	{
		case CODEC_SEND:
			return c->normalizes;
		case CODEC_ARRAY:
			return prolly_datum_codec_normalizes(c->elem.codec);
		case CODEC_COMPOSITE:
			for (int i = 0; i < c->tupdesc->natts; i++)
				if (c->attcodecs[i] && prolly_datum_codec_normalizes(c->attcodecs[i]))
					return true;
			return false;
		case CODEC_ANYARRAY:
			return true;
		default:
			return false;
	}
}

bool
prolly_datum_codec_checks(const ProllyDatumCodec *c)
{
	switch (c->kind)
	{
		case CODEC_OUTPUT:
			return true;
		case CODEC_ARRAY:
			return prolly_datum_codec_checks(c->elem.codec);
		case CODEC_COMPOSITE:
			for (int i = 0; i < c->tupdesc->natts; i++)
				if (c->attcodecs[i] && prolly_datum_codec_checks(c->attcodecs[i]))
					return true;
			return false;
		default:
			return false;
	}
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
		case CODEC_FLOAT4:
			put_be(out, float4_bits(DatumGetFloat4(value)), 4);
			break;
		case CODEC_FLOAT8:
			put_be(out, float8_bits(DatumGetFloat8(value)), 8);
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
		case CODEC_OUTPUT:
			encode_output(c, value, out);
			break;
		case CODEC_SEND:
			{
				Datum		canon = send_floats(c, value);
				bytea	   *b = send_utf8(&c->send, canon);

				/*
				 * Extension binary input can normalize representations that
				 * binary output distinguishes (for example PostGIS NaN points).
				 * Store the representation we will get back, not a pre-receive
				 * spelling. initdb's fixed I/O and our explicit built-in codecs
				 * need no extra round trip; ranges whose bounds hold extension
				 * types do.
				 */
				if (c->normalizes)
				{
					Datum		roundtrip = recv_utf8(c, VARDATA(b),
													  VARSIZE(b) - VARHDRSZ);
					bytea	   *normalized = send_utf8(&c->send, roundtrip);

					pfree(b);
					b = normalized;
					if (!c->typbyval)
						pfree(DatumGetPointer(roundtrip));
				}
				if (c->tagged)
					appendStringInfoChar(out, TAG_SEND);
				appendBinaryStringInfo(out, VARDATA(b), VARSIZE(b) - VARHDRSZ);
				pfree(b);
				if (canon != value)
					pfree(DatumGetPointer(canon));
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
		case CODEC_FLOAT4:
			{
				uint32		bits;
				float4		f;

				if (len != 4)
					bad_value("float4 is not 4 bytes");
				bits = (uint32) get_be(data, 4);
				memcpy(&f, &bits, sizeof(f));
				return Float4GetDatum(f);
			}
		case CODEC_FLOAT8:
			{
				uint64		bits;
				float8		f;

				if (len != 8)
					bad_value("float8 is not 8 bytes");
				bits = get_be(data, 8);
				memcpy(&f, &bits, sizeof(f));
				return Float8GetDatum(f);
			}
		case CODEC_TEXT:
			/* (as text's receive does: a commit's bytes aren't trusted) */
			(void) pg_verify_mbstr(PG_UTF8, data, len, false);
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
			if (c->enum_mapping)
			{
				char	   *label = pnstrdup(data, len);
				const char *mapped;
				char	   *utf8;

				if (strlen(label) != len)
					bad_value("enum label contains a zero byte");
				label = pg_any_to_server(label, len, PG_UTF8);
				mapped = historical_enum_label(c, label);
				utf8 = pg_server_to_any(mapped, strlen(mapped), PG_UTF8);
				return recv_utf8(c, utf8, strlen(utf8));
			}
			if (c->tagged)
			{
				if (len < 1)
					bad_value("a tagged value without its tag");
				if ((uint8) data[0] == TAG_OUTPUT)
					return decode_text(c, data + 1, len - 1);
				if ((uint8) data[0] != TAG_SEND)
					bad_value("an unknown tag");
				return recv_utf8(c, data + 1, len - 1);
			}
			return recv_utf8(c, data, len);
		case CODEC_OUTPUT:
			if (len < 1)
				bad_value("a tagged value without its tag");
			if ((uint8) data[0] == TAG_OUTPUT)
				return decode_text(c, data + 1, len - 1);
			if ((uint8) data[0] != TAG_SEND)
				bad_value("an unknown tag");
			if (!c->has_recv)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a value of type %s was stored in its binary form, and the type has no binary receive function now",
								format_type_be(c->type))));
			return recv_utf8(c, data + 1, len - 1);
		default:
			elog(ERROR, "unrecognized prolly datum codec kind %d", c->kind);
			return (Datum) 0;	/* keep compiler quiet */
	}
	if (p != end)
		bad_value("trailing bytes");
	return result;
}
