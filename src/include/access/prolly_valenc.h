/*-------------------------------------------------------------------------
 *
 * prolly_valenc.h
 *	  Value encoder interface: turning rows into bytes and back.
 *
 * Whether rows use Postgres's tuple format (so executor deforming works
 * unchanged) or a portable canonical encoding is open question 12.  Canonical
 * chunks must be identical across platforms either way.
 *
 * Contract:
 * - decode(encode(row)) gives back the same attribute values.
 * - Encodings that appear in canonical chunks are identical on every
 *   platform.
 * - Values arrive detoasted; storing large values out of line is the
 *   engine's job.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_valenc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_VALENC_H
#define PROLLY_VALENC_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"

typedef struct ProllyValueEncoderRoutine
{
	ProllyImplInfo info;

	ProllyValueEncoder *(*begin) (TupleDesc desc, MemoryContext mcxt);

	/* Append the encoding of slot's row to out. */
	void		(*encode) (ProllyValueEncoder *enc, TupleTableSlot *slot,
						   StringInfo out);

	/* Store the row encoded in value into slot (as a virtual tuple). */
	void		(*decode) (ProllyValueEncoder *enc, const ProllyValue *value,
						   TupleTableSlot *slot);

	void		(*end) (ProllyValueEncoder *enc);
} ProllyValueEncoderRoutine;

#define PROLLY_VALENC_CALLBACK_NAMES "begin", "encode", "decode", "end"
#define PROLLY_VALENC_FIRST_CALLBACK begin

static inline const ProllyValueEncoderRoutine *
prolly_valenc_lookup(const char *name)
{
	return (const ProllyValueEncoderRoutine *)
		prolly_get_impl(PROLLY_IFACE_VALUE_ENCODER, name);
}

#endif							/* PROLLY_VALENC_H */
