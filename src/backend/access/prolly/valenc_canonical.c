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
 * A layout is the list of attribute numbers rows are encoded over (int16
 * each, native byte order: layouts are local).  DROP COLUMN doesn't
 * rewrite the table, so rows encoded before it still hold the column;
 * begin_layout() decodes them, skipping values of columns dropped since
 * and filling columns added since with their missing value or NULL.  Rows
 * encoded under a layout decode correctly with the current encoder while
 * that layout is a prefix of the current one (columns were only added).
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
	int		   *live_attno;		/* 0-based attribute index of encoded column j */
	bool	   *has_missing;	/* per encoded column */
	bool	   *nulls;			/* scratch, nlive entries */

	/* Decoding an earlier layout: live columns it doesn't hold. */
	bool		decode_only;
	int			nextra;
	int		   *extra_attno;
};

/*
 * An encoder over the given 0-based attribute indexes (NULL: the live
 * columns); indexes of columns dropped since get no codec and are skipped.
 */
static ProllyValueEncoder *
begin_columns(TupleDesc desc, const int *attnos, int nattnos, MemoryContext mcxt)
{
	ProllyValueEncoder *enc = MemoryContextAllocZero(mcxt, sizeof(ProllyValueEncoder));
	int			natts = desc->natts;
	int			ncols = attnos ? nattnos : natts;
	bool	   *covered = palloc0(sizeof(bool) * Max(natts, 1));

	enc->desc = desc;
	enc->tmpcxt = AllocSetContextCreate(mcxt, "prolly canonical rows",
										ALLOCSET_DEFAULT_SIZES);
	enc->codecs = MemoryContextAllocZero(mcxt, sizeof(ProllyDatumCodec *) * Max(natts, 1));
	enc->live_attno = MemoryContextAlloc(mcxt, sizeof(int) * Max(ncols, 1));
	enc->has_missing = MemoryContextAlloc(mcxt, sizeof(bool) * Max(ncols, 1));
	enc->nulls = MemoryContextAlloc(mcxt, sizeof(bool) * Max(ncols, 1));
	enc->extra_attno = MemoryContextAlloc(mcxt, sizeof(int) * Max(natts, 1));
	enc->decode_only = (attnos != NULL);

	for (int j = 0; j < ncols; j++)
	{
		int			i = attnos ? attnos[j] : j;
		Form_pg_attribute att;

		if (i < 0 || i >= natts || (attnos && j > 0 && i <= attnos[j - 1]))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid canonical prolly row layout")));
		att = TupleDescAttr(desc, i);
		if (att->attisdropped)
		{
			if (!attnos)
				continue;
			/* Dropped since the rows were encoded: skipped on decode. */
			enc->live_attno[enc->nlive] = i;
			enc->has_missing[enc->nlive] = false;
			enc->nlive++;
			continue;
		}
		enc->codecs[i] = prolly_datum_codec(att->atttypid, att->atttypmod, mcxt);
		enc->live_attno[enc->nlive] = i;
		enc->has_missing[enc->nlive] = att->atthasmissing;
		enc->nlive++;
		covered[i] = true;
	}
	for (int i = 0; attnos && i < natts; i++)
	{
		if (!covered[i] && !TupleDescAttr(desc, i)->attisdropped)
			enc->extra_attno[enc->nextra++] = i;
	}
	pfree(covered);

	/* A layout that is the current one encodes like begin() would. */
	if (attnos && enc->nextra == 0)
	{
		bool		current = true;

		for (int j = 0; j < enc->nlive; j++)
			if (enc->codecs[enc->live_attno[j]] == NULL)
				current = false;
		enc->decode_only = !current;
	}
	return enc;
}

static ProllyValueEncoder *
canonical_begin(TupleDesc desc, MemoryContext mcxt)
{
	return begin_columns(desc, NULL, 0, mcxt);
}

static void
canonical_layout(ProllyValueEncoder *enc, StringInfo out)
{
	for (int j = 0; j < enc->nlive; j++)
	{
		int16		attno = (int16) enc->live_attno[j];

		appendBinaryStringInfo(out, (const char *) &attno, sizeof(attno));
	}
}

static int *
parse_layout(const ProllyValue *layout, int *n)
{
	int		   *attnos;

	if (layout->len % sizeof(int16) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row layout")));
	*n = layout->len / sizeof(int16);
	attnos = palloc(sizeof(int) * Max(*n, 1));
	for (int j = 0; j < *n; j++)
	{
		int16		a;

		memcpy(&a, layout->data + j * sizeof(int16), sizeof(a));
		attnos[j] = a;
	}
	return attnos;
}

static ProllyValueEncoder *
canonical_begin_layout(TupleDesc desc, const ProllyValue *layout,
					   MemoryContext mcxt)
{
	int			n;
	int		   *attnos = parse_layout(layout, &n);
	ProllyValueEncoder *enc = begin_columns(desc, attnos, n, mcxt);

	pfree(attnos);
	return enc;
}

static void
canonical_encode(ProllyValueEncoder *enc, TupleTableSlot *slot, StringInfo out)
{
	MemoryContext old;
	int			n = 0;

	if (enc->decode_only)
		elog(ERROR, "a prolly encoder for an earlier row layout can only decode");
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

			/* No codec: the column was dropped after the row was encoded. */
			if (enc->codecs[i] != NULL)
			{
				slot->tts_values[i] = prolly_datum_decode(enc->codecs[i], p, (int) len);
				slot->tts_isnull[i] = false;
			}
			p += len;
		}
	}

	/* Columns added after the rows' layout. */
	for (int x = 0; x < enc->nextra; x++)
	{
		int			i = enc->extra_attno[x];

		if (TupleDescAttr(desc, i)->atthasmissing)
			slot->tts_values[i] = getmissingattr(desc, i + 1, &slot->tts_isnull[i]);
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
	pfree(enc->extra_attno);
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
	.layout = canonical_layout,
	.begin_layout = canonical_begin_layout,
	.encode = canonical_encode,
	.decode = canonical_decode,
	.end = canonical_end,
};
