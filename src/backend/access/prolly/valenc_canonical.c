/*-------------------------------------------------------------------------
 *
 * valenc_canonical.c
 *	  Value encoder implementation "canonical": portable rows that depend
 *	  only on their logical content.
 *
 * A row's bytes must not depend on platform, cluster, session or the DDL
 * history of its table.  A row is encoded as
 *
 *	varint n		number of live (not dropped) columns stored
 *	null bitmap		n bits, 1 = NULL
 *	values			each non-NULL column as varint length + its canonical
 *					bytes (prolly_datum.h)
 *
 * over the live columns in attribute order.  Dropped columns are never
 * stored, so a table that dropped a column encodes rows exactly as one that
 * never had it.  Trailing NULL columns are not stored, so adding a nullable
 * column without a default needs no rewrite.
 *
 * Columns added with a default (atthasmissing) are the exception: the
 * default lives in the catalog until the table is rewritten.  A column that
 * is absent from a row (the row predates the column) decodes to its missing
 * value; so rows written while the column has one store an explicit NULL
 * for it rather than leaving it off.  Such rows are not yet canonical; the
 * version-control commit will rewrite the table and clear the missing
 * values, after which they are.
 *
 * Decoding fills a slot in the relation's own layout, with NULLs for dropped
 * columns, as Postgres's tuple conversion would.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/valenc_canonical.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "utils/memutils.h"

struct ProllyValueEncoder
{
	TupleDesc	desc;
	MemoryContext tmpcxt;		/* reset after each row */
	ProllyDatumCodec **codecs;	/* per attribute; NULL if dropped */
	int			nlive;
	int		   *live_attno;		/* 0-based attribute index of live column j */
	bool	   *has_missing;	/* per live column */
	bool	   *nulls;			/* scratch, nlive entries */
};

static ProllyValueEncoder *
canonical_begin(TupleDesc desc, MemoryContext mcxt)
{
	ProllyValueEncoder *enc = MemoryContextAllocZero(mcxt, sizeof(ProllyValueEncoder));
	int			natts = desc->natts;

	enc->desc = desc;
	enc->tmpcxt = AllocSetContextCreate(mcxt, "prolly canonical rows",
										ALLOCSET_DEFAULT_SIZES);
	enc->codecs = MemoryContextAllocZero(mcxt, sizeof(ProllyDatumCodec *) * natts);
	enc->live_attno = MemoryContextAlloc(mcxt, sizeof(int) * Max(natts, 1));
	enc->has_missing = MemoryContextAlloc(mcxt, sizeof(bool) * Max(natts, 1));
	enc->nulls = MemoryContextAlloc(mcxt, sizeof(bool) * Max(natts, 1));

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		enc->codecs[i] = prolly_datum_codec(att->atttypid, att->atttypmod, mcxt);
		enc->live_attno[enc->nlive] = i;
		enc->has_missing[enc->nlive] = att->atthasmissing;
		enc->nlive++;
	}
	return enc;
}

static void
canonical_encode(ProllyValueEncoder *enc, TupleTableSlot *slot, StringInfo out)
{
	MemoryContext old;
	int			n = 0;

	slot_getallattrs(slot);

	/* Columns up to the last that is non-NULL or has a missing value. */
	for (int j = 0; j < enc->nlive; j++)
	{
		int			i = enc->live_attno[j];

		enc->nulls[j] = slot->tts_isnull[i];
		if (!slot->tts_isnull[i] || enc->has_missing[j])
			n = j + 1;
	}

	old = MemoryContextSwitchTo(enc->tmpcxt);
	prolly_put_varint(out, n);
	for (int j = 0; j < n; j += 8)
	{
		uint8		b = 0;

		for (int k = 0; k < 8 && j + k < n; k++)
			if (enc->nulls[j + k])
				b |= 1 << k;
		appendStringInfoChar(out, (char) b);
	}
	for (int j = 0; j < n; j++)
	{
		int			i = enc->live_attno[j];
		StringInfoData val;

		if (enc->nulls[j])
			continue;
		initStringInfo(&val);
		prolly_datum_encode(enc->codecs[i], slot->tts_values[i], &val);
		prolly_put_varint(out, val.len);
		appendBinaryStringInfo(out, val.data, val.len);
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(enc->tmpcxt);
}

static void
canonical_decode(ProllyValueEncoder *enc, const ProllyValue *value,
				 TupleTableSlot *slot)
{
	TupleDesc	desc = enc->desc;
	const char *p = (const char *) value->data;
	const char *end = p + value->len;
	MemoryContext old;
	int			n;

	ExecClearTuple(slot);
	for (int i = 0; i < desc->natts; i++)
	{
		slot->tts_values[i] = (Datum) 0;
		slot->tts_isnull[i] = true;
	}

	old = MemoryContextSwitchTo(enc->tmpcxt);
	n = (int) prolly_get_varint(&p, end);
	if (n > enc->nlive || end - p < (n + 7) / 8)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row header")));
	for (int j = 0; j < n; j++)
		enc->nulls[j] = ((uint8) p[j / 8] >> (j % 8)) & 1;
	p += (n + 7) / 8;

	for (int j = 0; j < enc->nlive; j++)
	{
		int			i = enc->live_attno[j];

		if (j >= n)
		{
			/* Absent: the row predates the column. */
			if (enc->has_missing[j])
				slot->tts_values[i] = getmissingattr(desc, i + 1,
													 &slot->tts_isnull[i]);
			continue;
		}
		if (!enc->nulls[j])
		{
			uint64		len = prolly_get_varint(&p, end);

			if (len > (uint64) (end - p))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid canonical prolly row: value runs past the end")));
			slot->tts_values[i] = prolly_datum_decode(enc->codecs[i], p, (int) len);
			slot->tts_isnull[i] = false;
			p += len;
		}
	}
	if (p != end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row: %d trailing bytes",
						(int) (end - p))));

	MemoryContextSwitchTo(old);
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);	/* copy values out of tmpcxt */
	MemoryContextReset(enc->tmpcxt);
}

static void
canonical_end(ProllyValueEncoder *enc)
{
	MemoryContextDelete(enc->tmpcxt);
	pfree(enc->codecs);
	pfree(enc->live_attno);
	pfree(enc->has_missing);
	pfree(enc->nulls);
	pfree(enc);
}

const ProllyValueEncoderRoutine prolly_valenc_canonical = {
	.info = {
		.iface = PROLLY_IFACE_VALUE_ENCODER,
		.impl_id = 2,
		.format_version = 1,
		.name = "canonical",
		.description = "portable rows of live columns, trailing NULLs omitted, "
		"no local OIDs",
	},
	.begin = canonical_begin,
	.encode = canonical_encode,
	.decode = canonical_decode,
	.end = canonical_end,
};
