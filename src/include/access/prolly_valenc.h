/*-------------------------------------------------------------------------
 *
 * prolly_valenc.h
 *	  Value encoder interface: turning rows into bytes and back.
 *
 * Rows are stored in canonical form.
 *
 * Contract:
 * - decode(encode(row)) gives back the same attribute values.
 * - The bytes depend only on the row's logical content: not on the
 *   platform, the cluster, the session (client_encoding) or the table's
 *   DDL history (dropped columns, nullable columns added later), and they
 *   hold no local OIDs.  The one allowed exception: rows of a table with a
 *   column added with a default (atthasmissing) until the table is
 *   rewritten.
 * - Storing large values out of line is the engine's job.
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
