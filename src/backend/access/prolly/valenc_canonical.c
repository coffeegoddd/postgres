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
 *					bytes (prolly_datum.h), or, if those are longer than
 *					VALUE_INLINE_MAX, as varint VALUE_INLINE_MAX + 1 + the
 *					length of a reference to a value tree holding them, and
 *					the reference
 *
 * over the live columns in attribute order.  No value stored inline is
 * longer than VALUE_INLINE_MAX, so a length past it marks a reference.
 * Whether a value goes out of line depends on its own bytes alone, never
 * on its row or its table: a value encodes alike wherever it is, and two
 * rows holding it share its tree.  A row without long values is the same
 * bytes as before references were.
 *
 * A value stored by content (a hash pointer, prolly_toast.h) whose tree
 * holds its canonical bytes is that tree: its reference is written from
 * the pointer, and the value isn't read (one short enough to be inline is,
 * and is stored inline).  Decoding gives a long value whose tree a hash
 * pointer can name as that pointer, unread (prolly_toast_pointer()): a row
 * written from history holds the tree history has, and a value no one
 * reads is never fetched.  A value TOAST stored out of line in a TOAST
 * relation is looked up in the value store's memo
 * by its pointer first: a fold that meets a pointer an UPDATE of another
 * column kept writes the reference the fold before made, without reading
 * the value.  A miss reads it, and remembers the reference made.  Dropped columns are never
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
#include "access/prolly_toast.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "varatt.h"

/* longer canonical bytes are stored out of line; part of the format */
#define VALUE_INLINE_MAX	2048

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

	const ProllyValueStore *values; /* values out of line; NULL if none */
	StringInfoData scratch;		/* a value read from out of line */
};

static void
no_values(void)
{
	elog(ERROR, "a prolly row has a value out of line, and no value store was given");
}

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
	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		initStringInfo(&enc->scratch);
		MemoryContextSwitchTo(old);
	}

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
		Form_pg_attribute att = TupleDescAttr(enc->desc, i);
		const void *toasted = NULL;
		StringInfoData val;
		StringInfoData ref;

		if (enc->nulls[j])
			continue;

		/*
		 * A value stored by content: its tree is the column's, when it holds
		 * the canonical bytes and is too long to be inline.  Not read.
		 */
		if (att->attlen == -1 && enc->values &&
			VARATT_IS_EXTERNAL_ONDISK_HASH(DatumGetPointer(slot->tts_values[i])))
		{
			uint64		len;

			initStringInfo(&ref);
			if (prolly_toast_canonical((varlena *) DatumGetPointer(slot->tts_values[i]),
									   att->atttypid, enc->values, &ref, &len) &&
				len > VALUE_INLINE_MAX)
			{
				prolly_put_varint(out, VALUE_INLINE_MAX + 1 + (uint64) ref.len);
				appendBinaryStringInfo(out, ref.data, ref.len);
				continue;
			}
		}

		/* A value TOAST stored out of line: its reference, if remembered. */
		if (att->attlen == -1 && enc->values && enc->values->memo &&
			VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(slot->tts_values[i])))
		{
			toasted = DatumGetPointer(slot->tts_values[i]);
			initStringInfo(&ref);
			if (PROLLY_CALL(enc->values->memo, lookup, enc->values, toasted,
							att->atttypid, &ref))
			{
				prolly_put_varint(out, VALUE_INLINE_MAX + 1 + (uint64) ref.len);
				appendBinaryStringInfo(out, ref.data, ref.len);
				continue;
			}
		}

		initStringInfo(&val);
		prolly_datum_encode(enc->codecs[i], slot->tts_values[i], &val);
		if (val.len > VALUE_INLINE_MAX)
		{
			if (enc->values == NULL)
				no_values();
			if (toasted)
				resetStringInfo(&ref);	/* (the lookup's) */
			else
				initStringInfo(&ref);
			PROLLY_CALL(enc->values->tree, value_write, enc->values->env,
						(const uint8 *) val.data, val.len, &ref);
			prolly_put_varint(out, VALUE_INLINE_MAX + 1 + (uint64) ref.len);
			appendBinaryStringInfo(out, ref.data, ref.len);
			if (toasted)
			{
				ProllyValue r = {(const uint8 *) ref.data, ref.len};

				PROLLY_CALL(enc->values->memo, remember, toasted,
							att->atttypid, &r);
			}
		}
		else
		{
			prolly_put_varint(out, val.len);
			appendBinaryStringInfo(out, val.data, val.len);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(enc->tmpcxt);
}

/* Called with each out-of-line value of a row: its column and reference. */
typedef void (*RowRefVisit) (int column, const char *ref, uint64 len,
							 void *arg);

/*
 * Visit the references to value trees an encoded row holds, checking the
 * row is whole: a header its bytes hold, values within it, nothing after
 * them.  (A row of an earlier layout may have more columns than enc's.)
 * Returns how many columns the row stores.
 */
static int
walk_row_refs(const ProllyValue *row, RowRefVisit visit, void *arg)
{
	const char *p = (const char *) row->data;
	const char *end = p + row->len;
	const char *nulls;
	uint64		n = prolly_get_varint(&p, end);

	if ((uint64) (end - p) < (n + 7) / 8)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row header")));
	nulls = p;
	p += (n + 7) / 8;
	for (uint64 j = 0; j < n; j++)
	{
		uint64		len;
		bool		out_of_line;

		if (((uint8) nulls[j / 8] >> (j % 8)) & 1)
			continue;
		len = prolly_get_varint(&p, end);
		out_of_line = len > VALUE_INLINE_MAX;
		if (out_of_line)
			len -= VALUE_INLINE_MAX + 1;
		if (len > (uint64) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid canonical prolly row: value runs past the end")));
		if (out_of_line)
			visit((int) j, p, len, arg);
		p += len;
	}
	if (p != end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row: %d trailing bytes",
						(int) (end - p))));
	return (int) n;
}

static void
note_column_ref(int column, const char *ref, uint64 len, void *arg)
{
	ProllyValue *refs = arg;

	refs[column].data = (const uint8 *) ref;
	refs[column].len = (uint32) len;
}

/*
 * The references to value trees an encoded row of enc's layout holds, by
 * encoded column (empty where a column has none): pointers into row, in
 * the current memory context.  n gets how many columns the row stores.
 */
static ProllyValue *
row_refs_by_column(ProllyValueEncoder *enc, const ProllyValue *row, int *n)
{
	ProllyValue *refs = palloc0(sizeof(ProllyValue) * Max(enc->nlive, 1));
	const char *p = (const char *) row->data;

	if (prolly_get_varint(&p, p + row->len) > (uint64) enc->nlive)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row header")));
	*n = walk_row_refs(row, note_column_ref, refs);
	return refs;
}

/*
 * Decode value into slot; with before, each column whose value is out of
 * line with the same reference in before is left NULL, unread, and marked
 * in kept.
 */
static void
decode_into(ProllyValueEncoder *enc, const ProllyValue *value,
			const ProllyValue *before, TupleTableSlot *slot, bool *kept)
{
	TupleDesc	desc = enc->desc;
	const char *p = (const char *) value->data;
	const char *end = p + value->len;
	MemoryContext old;
	int			n;
	ProllyValue *brefs = NULL;
	int			nb = 0;

	ExecClearTuple(slot);
	for (int i = 0; i < desc->natts; i++)
	{
		slot->tts_values[i] = (Datum) 0;
		slot->tts_isnull[i] = true;
	}

	old = MemoryContextSwitchTo(enc->tmpcxt);
	if (before)
	{
		memset(kept, 0, sizeof(bool) * desc->natts);
		brefs = row_refs_by_column(enc, before, &nb);
	}
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
			bool		out_of_line = len > VALUE_INLINE_MAX;

			if (out_of_line)
				len -= VALUE_INLINE_MAX + 1;
			if (len > (uint64) (end - p))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid canonical prolly row: value runs past the end")));

			/* No codec: the column was dropped after the row was encoded. */
			if (enc->codecs[i] != NULL && out_of_line && j < nb &&
				brefs[j].len == len && memcmp(brefs[j].data, p, len) == 0)
				kept[i] = true;
			else if (enc->codecs[i] != NULL && out_of_line)
			{
				ProllyValue ref = {(const uint8 *) p, (uint32) len};
				ProllyHash	root;
				uint64		vlen;
				varlena    *pointer;

				if (enc->values == NULL)
					no_values();

				/* a tree a hash pointer can name: named, not read */
				PROLLY_CALL(enc->values->tree, value_root, &ref, &root, &vlen);
				pointer = prolly_toast_pointer(&root, vlen,
											   TupleDescAttr(desc, i)->atttypid);
				if (pointer != NULL)
					slot->tts_values[i] = PointerGetDatum(pointer);
				else
				{
					INJECTION_POINT("prolly-value-read", NULL);
					resetStringInfo(&enc->scratch);
					PROLLY_CALL(enc->values->tree, value_read, enc->values->env,
								&ref, 0, PG_UINT64_MAX, &enc->scratch);
					slot->tts_values[i] = prolly_datum_decode(enc->codecs[i],
															  enc->scratch.data,
															  enc->scratch.len);
				}
				slot->tts_isnull[i] = false;
			}
			else if (enc->codecs[i] != NULL)
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
	if (enc->scratch.maxlen > 64 * 1024)
	{
		/* (a long value's buffer isn't kept for the rows after it) */
		MemoryContext cxt = MemoryContextSwitchTo(GetMemoryChunkContext(enc));

		pfree(enc->scratch.data);
		initStringInfo(&enc->scratch);
		MemoryContextSwitchTo(cxt);
	}
}

static void
canonical_decode(ProllyValueEncoder *enc, const ProllyValue *value,
				 TupleTableSlot *slot)
{
	decode_into(enc, value, NULL, slot, NULL);
}

static void
canonical_decode_kept(ProllyValueEncoder *enc, const ProllyValue *value,
					  const ProllyValue *before, TupleTableSlot *slot,
					  bool *kept)
{
	decode_into(enc, value, before, slot, kept);
}

static void
canonical_set_values(ProllyValueEncoder *enc, const ProllyValueStore *values)
{
	enc->values = values;
}

static void
append_ref(int column, const char *ref, uint64 len, void *arg)
{
	appendBinaryStringInfo((StringInfo) arg, ref, (int) len);
}

static void
canonical_refs(ProllyValueEncoder *enc, const ProllyValue *row, StringInfo refs)
{
	(void) walk_row_refs(row, append_ref, refs);
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
	pfree(enc->scratch.data);
	pfree(enc);
}

const ProllyValueEncoderRoutine prolly_valenc_canonical = {
	.info = {
		.iface = PROLLY_IFACE_VALUE_ENCODER,
		.impl_id = 2,
		.format_version = 3,		/* 3: jsonb as stored */
		.name = "canonical",
		.description = "portable rows of live columns, trailing NULLs omitted, "
		"no local OIDs, long values out of line",
	},
	.begin = canonical_begin,
	.layout = canonical_layout,
	.begin_layout = canonical_begin_layout,
	.encode = canonical_encode,
	.decode = canonical_decode,
	.end = canonical_end,
	.set_values = canonical_set_values,
	.refs = canonical_refs,
	.decode_kept = canonical_decode_kept,
};
