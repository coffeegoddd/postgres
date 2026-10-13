/*-------------------------------------------------------------------------
 *
 * prolly_keyenc.h
 *	  Key encoder interface: turning index key columns into ordered bytes.
 *
 * Key order must agree with the btree opclasses and collations of the key
 * columns, which is what the planner and constraints assume.  Where an
 * encoding can be compared with memcmp, compare() is memcmp; otherwise it
 * falls back to the opclass comparator.
 *
 * Contract:
 * - compare(encode(a), encode(b)) has the sign of the opclass comparison of
 *   a and b, column by column, honouring DESC and NULLS FIRST/LAST.
 * - Encodings are part of the canonical format: identical on every
 *   platform, in every cluster and in every session, with no local OIDs.
 * - decode() returns false if the encoding can't be reversed (e.g. collation
 *   sort keys); callers then read the column from the row.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_keyenc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_KEYENC_H
#define PROLLY_KEYENC_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"

/* The key columns to encode, as an index describes them. */
typedef struct ProllyKeyDesc
{
	int			nkeys;
	const Oid  *types;
	const Oid  *opfamilies;		/* btree opfamilies */
	const Oid  *collations;
	const int16 *options;		/* INDOPTION_DESC, INDOPTION_NULLS_FIRST */
} ProllyKeyDesc;

typedef struct ProllyKeyEncoderRoutine
{
	ProllyImplInfo info;

	/* True if this implementation can encode type under opfamily/collation. */
	bool		(*supports) (Oid opfamily, Oid type, Oid collation);

	ProllyKeyEncoder *(*begin) (const ProllyKeyDesc *desc, MemoryContext mcxt);

	/* Append the encoding of the key columns to out. */
	void		(*encode) (ProllyKeyEncoder *enc, const Datum *values,
						   const bool *isnull, StringInfo out);

	bool		(*decode) (ProllyKeyEncoder *enc, const ProllyKey *key,
						   Datum *values, bool *isnull);

	int			(*compare) (ProllyKeyEncoder *enc, const ProllyKey *a,
							const ProllyKey *b);

	void		(*end) (ProllyKeyEncoder *enc);
} ProllyKeyEncoderRoutine;

#define PROLLY_KEYENC_CALLBACK_NAMES \
	"supports", "begin", "encode", "decode", "compare", "end"
#define PROLLY_KEYENC_FIRST_CALLBACK supports

static inline const ProllyKeyEncoderRoutine *
prolly_keyenc_lookup(const char *name)
{
	return (const ProllyKeyEncoderRoutine *)
		prolly_get_impl(PROLLY_IFACE_KEY_ENCODER, name);
}

#endif							/* PROLLY_KEYENC_H */
