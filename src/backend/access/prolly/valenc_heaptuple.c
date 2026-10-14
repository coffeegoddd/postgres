/*-------------------------------------------------------------------------
 *
 * valenc_heaptuple.c
 *	  Value encoder implementation "heaptuple": rows as minimal tuples.
 *
 * A row is stored as the bytes of a MinimalTuple (header, null bitmap and
 * attributes in Postgres's on-disk tuple layout), so decoding is Postgres's
 * own tuple deforming.
 *
 * Not the default: kept for comparison with "canonical".  Its bytes depend on the platform (byte order,
 * alignment) and on the table's history (dropped columns stay in tuples,
 * added columns' defaults live in the catalog), and enum, array and
 * composite values carry local OIDs, so it fails the value encoder's
 * platform and history conformance checks.
 *
 * Values must arrive detoasted (prolly_valenc.h); an external TOAST pointer
 * is an error.  Compressed inline values are stored as they are.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/valenc_heaptuple.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "varatt.h"

struct ProllyValueEncoder
{
	TupleDesc	desc;
	MemoryContext mcxt;
};

static ProllyValueEncoder *
heaptuple_begin(TupleDesc desc, MemoryContext mcxt)
{
	ProllyValueEncoder *enc = MemoryContextAlloc(mcxt, sizeof(ProllyValueEncoder));

	enc->desc = desc;
	enc->mcxt = mcxt;
	return enc;
}

/* Heap tuples keep dropped columns: the layout is the attribute count. */
static void
heaptuple_layout(ProllyValueEncoder *enc, StringInfo out)
{
	int16		natts = (int16) enc->desc->natts;

	appendBinaryStringInfo(out, (const char *) &natts, sizeof(natts));
}

static int16
layout_natts(const ProllyValue *layout)
{
	int16		natts;

	if (layout->len != sizeof(int16))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly heaptuple layout")));
	memcpy(&natts, layout->data, sizeof(natts));
	return natts;
}

/* Fewer attributes decode as missing or NULL, as for heap. */
static bool
heaptuple_reads_layout(ProllyValueEncoder *enc, const ProllyValue *layout)
{
	return layout_natts(layout) <= enc->desc->natts;
}

static ProllyValueEncoder *
heaptuple_begin_layout(TupleDesc desc, const ProllyValue *layout,
					   MemoryContext mcxt)
{
	if (layout_natts(layout) > desc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly rows have more attributes than their table")));
	return heaptuple_begin(desc, mcxt);
}

static void
heaptuple_encode(ProllyValueEncoder *enc, TupleTableSlot *slot, StringInfo out)
{
	TupleDesc	desc = enc->desc;
	MinimalTuple mtup;

	slot_getallattrs(slot);
	for (int i = 0; i < desc->natts; i++)
	{
		if (!slot->tts_isnull[i] && TupleDescCompactAttr(desc, i)->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(slot->tts_values[i])))
			elog(ERROR, "prolly value encoder received a toasted value "
				 "for column %d", i + 1);
	}

	mtup = heap_form_minimal_tuple(desc, slot->tts_values, slot->tts_isnull, 0);
	appendBinaryStringInfo(out, mtup, mtup->t_len);
	pfree(mtup);
}

static void
heaptuple_decode(ProllyValueEncoder *enc, const ProllyValue *value,
				 TupleTableSlot *slot)
{
	MinimalTuple mtup;

	if (value->len < SizeofMinimalTupleHeader)
		elog(ERROR, "prolly value of %u bytes is too short for a tuple",
			 value->len);

	/* The bytes in a node are unaligned; deform from an aligned copy. */
	mtup = MemoryContextAlloc(slot->tts_mcxt, value->len);
	memcpy(mtup, value->data, value->len);
	if (mtup->t_len != value->len)
		elog(ERROR, "prolly value length %u does not match its tuple length %u",
			 value->len, mtup->t_len);

	ExecForceStoreMinimalTuple(mtup, slot, true);
}

static void
heaptuple_end(ProllyValueEncoder *enc)
{
	pfree(enc);
}

const ProllyValueEncoderRoutine prolly_valenc_heaptuple = {
	.info = {
		.iface = PROLLY_IFACE_VALUE_ENCODER,
		.impl_id = 1,
		.format_version = 1,
		.name = "heaptuple",
		.description = "rows as MinimalTuple bytes (platform-dependent)",
	},
	.begin = heaptuple_begin,
	.layout = heaptuple_layout,
	.begin_layout = heaptuple_begin_layout,
	.reads_layout = heaptuple_reads_layout,
	.encode = heaptuple_encode,
	.decode = heaptuple_decode,
	.end = heaptuple_end,
};
