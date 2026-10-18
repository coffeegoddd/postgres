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
 *	jsonb			a version byte (1), then the document as jsonb stores it
 *					(after its varlena header), its strings in UTF-8: in a
 *					UTF-8 database, the bytes the value holds.
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

typedef struct ProllyDatumCodec ProllyDatumCodec;

/* A codec for values of typid/typmod; lives in mcxt. */
extern ProllyDatumCodec *prolly_datum_codec(Oid typid, int32 typmod,
											MemoryContext mcxt);

/* Append the canonical bytes of a non-NULL value. */
extern void prolly_datum_encode(ProllyDatumCodec *codec, Datum value,
								StringInfo out);

/*
 * The value whose canonical bytes are data[0..len); by-reference results
 * are allocated in CurrentMemoryContext.
 */
extern Datum prolly_datum_decode(ProllyDatumCodec *codec, const char *data,
								 int len);

/* Unsigned LEB128, the minimal encoding. */
extern void prolly_put_varint(StringInfo out, uint64 v);
extern uint64 prolly_get_varint(const char **p, const char *end);

#endif							/* PROLLY_DATUM_H */
