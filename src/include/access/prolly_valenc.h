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
 * - Postgres doesn't rewrite a table for every DDL change (DROP COLUMN,
 *   ADD COLUMN), so rows encoded under an earlier layout of the table
 *   remain.  layout() describes the layout rows are encoded with now (as
 *   bytes local to the implementation); begin_layout() makes an encoder
 *   that decodes rows encoded under an earlier layout into the table's
 *   current shape (encode() is an error with it); reads_layout() says
 *   whether rows encoded under a layout decode correctly with this
 *   encoder as it is.
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

	/* Append the layout this encoder encodes rows with. */
	void		(*layout) (ProllyValueEncoder *enc, StringInfo out);

	/* Decode rows encoded under layout into desc's shape. */
	ProllyValueEncoder *(*begin_layout) (TupleDesc desc,
										 const ProllyValue *layout,
										 MemoryContext mcxt);

	bool		(*reads_layout) (ProllyValueEncoder *enc,
								 const ProllyValue *layout);

	/* Append the encoding of slot's row to out. */
	void		(*encode) (ProllyValueEncoder *enc, TupleTableSlot *slot,
						   StringInfo out);

	/* Store the row encoded in value into slot (as a virtual tuple). */
	void		(*decode) (ProllyValueEncoder *enc, const ProllyValue *value,
						   TupleTableSlot *slot);

	void		(*end) (ProllyValueEncoder *enc);
} ProllyValueEncoderRoutine;

#define PROLLY_VALENC_CALLBACK_NAMES \
	"begin", "layout", "begin_layout", "reads_layout", "encode", "decode", \
	"end"
#define PROLLY_VALENC_FIRST_CALLBACK begin

static inline const ProllyValueEncoderRoutine *
prolly_valenc_lookup(const char *name)
{
	return (const ProllyValueEncoderRoutine *)
		prolly_get_impl(PROLLY_IFACE_VALUE_ENCODER, name);
}

#endif							/* PROLLY_VALENC_H */
