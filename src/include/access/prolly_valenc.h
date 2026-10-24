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
 * - A long value is stored out of line, as a reference to a value tree
 *   (value_write(), prolly_tree.h) in the encoder's value store, given by
 *   set_values(); which values are is fixed by the format, never by the
 *   row they are in, so a value encodes alike in every row.  Encoding or
 *   decoding a row that has one without a value store is an error.  refs()
 *   lists the references a row holds, for a tree's value_refs().  A whole
 *   row that is still long is stored out of line by the tree.
 * - Postgres doesn't rewrite a table for every DDL change (DROP COLUMN,
 *   ADD COLUMN), so rows encoded under an earlier layout of the table
 *   remain.  layout() describes the layout rows are encoded with now (as
 *   bytes local to the implementation); begin_layout() makes an encoder
 *   that decodes rows encoded under an earlier layout into the table's
 *   current shape (encode() is an error with it).
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
#include "access/prolly_tree.h"
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

	/* Append the encoding of slot's row to out. */
	void		(*encode) (ProllyValueEncoder *enc, TupleTableSlot *slot,
						   StringInfo out);

	/* Store the row encoded in value into slot (as a virtual tuple). */
	void		(*decode) (ProllyValueEncoder *enc, const ProllyValue *value,
						   TupleTableSlot *slot);

	void		(*end) (ProllyValueEncoder *enc);

	/* Store and read values out of line in values (kept, not copied). */
	void		(*set_values) (ProllyValueEncoder *enc,
							   const ProllyValueStore *values);

	/* Append the references to value trees an encoded row holds. */
	void		(*refs) (ProllyValueEncoder *enc, const ProllyValue *row,
						 StringInfo refs);

	/*
	 * decode(), except each column whose value is out of line with the same
	 * reference in row before (of the same layout): left NULL and unread,
	 * and marked in kept (by attribute index, false elsewhere), for the
	 * caller to take from wherever before's value is.
	 */
	void		(*decode_kept) (ProllyValueEncoder *enc, const ProllyValue *row,
								const ProllyValue *before,
								TupleTableSlot *slot, bool *kept);

	/*
	 * encode(), except each column marked in kept (by attribute index) is
	 * written as the reference to a value tree that row before (of this
	 * encoder's layout) holds there, its value in slot not read: a column
	 * decode_kept() left unread.  The reference names the value's canonical
	 * bytes, so the row is the one encode() would give with the value.
	 */
	void		(*encode_kept) (ProllyValueEncoder *enc, TupleTableSlot *slot,
								const ProllyValue *before, const bool *kept,
								StringInfo out);

	/*
	 * Optional: the columns an encoded row holds, by attribute index of the
	 * encoder's descriptor (fields gets natts entries), parsed in one pass
	 * and checked whole as decode() checks a row.  No value is decoded or
	 * read out of line.  A dropped column is not stored.  A column the row
	 * predates (absent from it, or added after its layout) is its missing
	 * value if it has one (missing: its nullness and canonical bytes, held
	 * inline whatever their length), else NULL.
	 */
	void		(*fields) (ProllyValueEncoder *enc, const ProllyValue *row,
						   ProllyEncodedField *fields);

	/*
	 * Optional: a field of attribute index attno that fields() gave, as a
	 * datum in the current memory context: its missing value or NULL if not
	 * stored, read from its value tree if out of line.
	 */
	Datum		(*decode_field) (ProllyValueEncoder *enc, int attno,
								 const ProllyEncodedField *field,
								 bool *isnull);

	/*
	 * Optional: whether two fields of attribute attno (fields() of two rows
	 * this encoder decodes) hold the same value, without decoding them:
	 * both NULL, or the same canonical bytes, whether a row stores the
	 * value or predates its column and has it as its missing value.  Equal
	 * bytes are usually enough, but a long missing value is held inline
	 * and a stored one by reference: that pair is compared by reading the
	 * stored value.
	 */
	bool		(*fields_alike) (ProllyValueEncoder *enc, int attno,
								 const ProllyEncodedField *a,
								 const ProllyEncodedField *b);

	/*
	 * Optional: whether fields of attribute attno of enc's rows and of
	 * other_attno of other's can be compared by fields_alike() (of either
	 * encoder): the same bytes are the same value, and different bytes
	 * different values.  False across a change of type, typmod or the
	 * type's definition (a column dropped and another added under its name
	 * is another attribute), or when either decodes enum labels through a
	 * historical mapping: compare decoded values then.
	 */
	bool		(*field_compatible) (ProllyValueEncoder *enc, int attno,
									 ProllyValueEncoder *other,
									 int other_attno);

	/*
	 * Optional: decode(), except only the attributes wanted marks (by
	 * attribute index; NULL: all of them).  The others are left NULL,
	 * neither decoded nor read out of line.  The row is still checked
	 * whole.
	 */
	void		(*decode_columns) (ProllyValueEncoder *enc,
								   const ProllyValue *row, const bool *wanted,
								   TupleTableSlot *slot);
} ProllyValueEncoderRoutine;

#define PROLLY_VALENC_CALLBACK_NAMES \
	"begin", "layout", "begin_layout", "encode", "decode", "end", \
	"set_values", "refs", "decode_kept", "encode_kept", "fields", \
	"decode_field", "fields_alike", "field_compatible", "decode_columns"
#define PROLLY_VALENC_FIRST_CALLBACK begin
#define PROLLY_VALENC_LAST_CALLBACK decode_columns

/* Historical enum decoding for the canonical encoder and nested values. */
extern bool prolly_canonical_map_enums(ProllyValueEncoder *enc, List *mappings);

static inline const ProllyValueEncoderRoutine *
prolly_valenc_lookup(const char *name)
{
	return (const ProllyValueEncoderRoutine *)
		prolly_get_impl(PROLLY_IFACE_VALUE_ENCODER, name);
}

#endif							/* PROLLY_VALENC_H */
