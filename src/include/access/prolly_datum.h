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
 *					Tag 0: send(receive(send(value))), with binary I/O in
 *					UTF-8; tag 1: its
 *					text form.  A value is read back by its tag, whatever
 *					the type's functions are now.
 *					Tag 1 is written for a type with send and receive when
 *					its receive refuses the value (a data exception, or an
 *					internal error its own code raises: PostGIS's refuses a
 *					one-point LINESTRING its text input takes).  Its text
 *					must then read back as a value sent the same, and the
 *					value is stored as its text reads back.
 *					A refusal is caught in a subtransaction, one for a
 *					fold's batch of values (prolly_datum_batch()), else
 *					one per value.  Not caught, so raised: in a parallel
 *					worker (a parallel index build), where none can be
 *					begun; while interrupts are held off (a buffer's
 *					content lock held); and for a range over a row type
 *					holding such a type, which is untagged.
 *					Receive may normalize different physical spellings to
 *					one value; keys and values store that normalized form.
 *					Keys additionally require it to compare equal to the
 *					original under the primary key's operator class, and
 *					values under other unique or exclusion indexes must
 *					reconstruct as values those indexes treat as the same.
 *	a type without binary send and receive (or a range over one)
 *					tag 1, then its text form (its output function's) in
 *					UTF-8, written with the settings it may depend on fixed
 *					(extra_float_digits 1, DateStyle ISO, MDY, IntervalStyle
 *					postgres, bytea_output hex, TimeZone UTC,
 *					timezone_abbreviations Default, lc_monetary, lc_numeric
 *					and lc_time C), read back with its input function and the
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
#include "utils/jsonb.h"
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

/* Whether binary receive may normalize this value, including its children. */
extern bool prolly_datum_codec_normalizes(const ProllyDatumCodec *c);

/*
 * Run a fold's encoding of many values (run(arg)) as one batch: an
 * extension's receive, which encoding a normalizing value calls, is called
 * unprotected inside one subtransaction, and if it refuses a value the
 * batch is rolled back and run again, each receive in a subtransaction of
 * its own, so the values refused are kept as their text form.  run must
 * be safe to run twice: everything it does between is rolled back.  A
 * batch inside a batch is run as part of it.  The batch runs in
 * prolly_datum_fixed_text().
 */
extern void prolly_datum_batch(void (*run) (void *arg), void *arg);

/*
 * Run run(arg) with the settings text forms are written and read under
 * fixed once (rather than once a value): for a loop encoding or decoding
 * many values.  Nothing it runs may depend on the session's own settings
 * (TimeZone, DateStyle, the locale's): it sees the fixed ones.  On an
 * error, the settings are restored before it is raised.  Nested, the inner
 * run is part of the outer.
 */
extern void prolly_datum_fixed_text(void (*run) (void *arg), void *arg);

/*
 * Set the settings prolly_datum_fixed_text() fixes, those not so already,
 * at the caller's GUC nest level (one it ends with AtEOXact_GUC()): for
 * code that already sets others so, as a fold sets search_path while it
 * describes definitions, whose constants print by these settings too.
 */
extern void prolly_datum_fix_text_settings(void);

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
 * Check that doc's len bytes (a jsonb document's binary, after its varlena
 * header) are a document jsonb builds, its strings in UTF-8, as canonical
 * jsonb bytes are checked: an error if not.
 */
extern void prolly_jsonb_check_utf8(const char *doc, uint32 len);

/*
 * jb, whose strings are in UTF-8, in the database's encoding: jb itself in
 * a UTF-8 database or when it is all ASCII, else a copy with its strings
 * converted (which can reorder its keys).
 */
extern Jsonb *prolly_jsonb_from_utf8(Jsonb *jb);

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
