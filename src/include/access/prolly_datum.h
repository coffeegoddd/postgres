/*-------------------------------------------------------------------------
 *
 * prolly_datum.h
 *	  Canonical encoding of single values (part of the canonical format).
 *
 * The bytes a value is stored as in canonical form: the same on every
 * platform, in every cluster and in every session, and free of local OIDs.
 * Key and value encoder implementations use this; it is a fixed part of the
 * canonical format, like the content hash's digest, not a component with
 * alternatives.
 *
 * The encoding of a value of type T:
 *
 *	domains			as their base type
 *	arrays			varint ndim; per dimension varint length and zigzag-varint
 *					lower bound; a null bitmap (one bit per element, 1 =
 *					NULL); each non-NULL element as varint length + its
 *					canonical bytes.  No element type OID: the column's type
 *					says what it is.
 *	composites		varint n (live attributes, trailing NULLs dropped); a
 *					null bitmap; each non-NULL attribute as varint length +
 *					canonical bytes.  No type OIDs.
 *	anyarray		the element type's qualified name (varint length, UTF-8),
 *					then the array (pg_statistic holds such values).
 *	float4, float8	the send format (IEEE 754 bits, network byte order),
 *					except that every NaN is the positive quiet NaN
 *					(7fc00000, 7ff8000000000000): NaNs are equal whatever
 *					their bits.  So too the floats inside geometric
 *					values, and ranges and multiranges of floats, which are
 *					otherwise their send format as below.
 *	jsonb			a version byte (2), then the document as jsonb stores it
 *					(after its varlena header), its strings in UTF-8: in a
 *					UTF-8 database, the bytes the value holds.  Version 1,
 *					jsonb's send format, is still read.
 *	a type whose binary I/O may change
 *					(a base type initdb didn't make, an extension's, or a
 *					range over one): a tag, then the value in that form.
 *					Tag 0: its binary send format, as below; tag 1: its
 *					text form.  A value is read back by its tag, whatever
 *					the type's functions are now.
 *	a type without binary send and receive (or a range over one)
 *					tag 1, then its text form (its output function's) in
 *					UTF-8, written with the settings it may depend on fixed
 *					(extra_float_digits 1, DateStyle ISO, MDY, IntervalStyle
 *					postgres), read back with its input function and the
 *					column's typmod.  A value whose text doesn't read back
 *					as the same value (datum_image_eq) isn't stored:
 *					encoding refuses it, and so do writes to a versioned
 *					table (prolly_check_row_key(), and a column's missing
 *					value when it is added).
 *	anything else	the type's binary send format, produced with the client
 *					encoding fixed to UTF-8, so text-like values are UTF-8
 *					whatever the session's client_encoding or the server
 *					encoding.  Send formats use network byte order.  Enums
 *					send their label.
 *
 * int2, int4, int8, bool and (in a UTF-8 or SQL_ASCII database) text,
 * varchar and bpchar are encoded directly, producing exactly the bytes their
 * send functions would, without the function call.
 *
 * Values of OID types (oid, regclass, …) are local by nature and are stored
 * as their send format for now; how canonical form should treat them is not
 * settled.
 *
 * src/include/access/prolly_datum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_DATUM_H
#define PROLLY_DATUM_H

#include "lib/stringinfo.h"
#include "nodes/pg_list.h"

typedef struct ProllyDatumCodec ProllyDatumCodec;

/*
 * A named enum read from a historical definition into today's type: each
 * saved label and the label it reads as (NULL: none, one added where the
 * enum read into has no such label).  targets NIL, with labels before: they
 * couldn't be mapped, refused where they'd be decoded.
 */
typedef struct ProllyEnumMapping
{
	Oid			type;
	List	   *before;			/* String nodes, in sort order */
	List	   *targets;		/* String nodes or NULLs, as before */
} ProllyEnumMapping;

extern bool prolly_datum_map_enums(ProllyDatumCodec *codec, List *mappings);

/* A codec for values of typid/typmod; lives in mcxt. */
extern ProllyDatumCodec *prolly_datum_codec(Oid typid, int32 typmod,
											MemoryContext mcxt);

/*
 * Whether encoding a value with c may refuse it: c holds (in an element or
 * attribute too) a type kept as its text form, whose value is refused where
 * its text doesn't read back as it.
 */
extern bool prolly_datum_codec_checks(const ProllyDatumCodec *c);

/* Append the canonical bytes of a non-NULL value. */
extern void prolly_datum_encode(ProllyDatumCodec *codec, Datum value,
								StringInfo out);

/*
 * The value whose canonical bytes are data[0..len); by-reference results
 * are allocated in CurrentMemoryContext.
 */
extern Datum prolly_datum_decode(ProllyDatumCodec *codec, const char *data,
								 int len);

/*
 * What values of typid encode by besides their type: the definitions their
 * canonical bytes depend on that the catalog can change without rewriting
 * the rows holding them (an enum's labels, a composite's attributes, and
 * those of the types they hold), appended to out.  Local, not canonical
 * (OIDs and attribute numbers): for telling whether bytes encoded before
 * are still the ones values encode to.
 */
extern void prolly_datum_definition(Oid typid, StringInfo out);

/*
 * Whether every value encoded under definition old (prolly_datum_definition(),
 * appended before) encodes to the same bytes under now: the same, or with
 * an enum's labels added, each old one kept (its OID and name), anywhere
 * in it.  (A label added keeps the old ones' order, so keys too.)
 */
extern bool prolly_datum_definition_extends(const char *old, int oldlen,
											const char *now, int nowlen);

/* Unsigned LEB128, the minimal encoding. */
extern void prolly_put_varint(StringInfo out, uint64 v);
extern uint64 prolly_get_varint(const char **p, const char *end);

#endif							/* PROLLY_DATUM_H */
