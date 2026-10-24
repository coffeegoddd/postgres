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
 * and is stored inline).  Decoding for rows written back to a table (the
 * value store's pointers) gives a long value whose tree a hash pointer can
 * name as that pointer, unread (prolly_toast_pointer()): the row holds the
 * tree history has, and a value no one reads is never fetched.  A value TOAST stored out of line in a TOAST
 * relation is looked up in the value store's memo
 * by its pointer first: a fold that meets a pointer an UPDATE of another
 * column kept writes the reference the fold before made, without reading
 * the value, if its type is defined as it was then (an enum's label
 * renamed, its values encode otherwise).  A miss reads it, and remembers
 * the reference made.  Dropped columns are never
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
#include "common/hashfn.h"
#include "pgstat.h"
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
	uint64	   *typdefs;		/* per attribute: its type's definition,
								 * hashed, for the memo; once asked */
	bool	   *typdef_known;
	StringInfoData scratch;		/* a value read from out of line */

	/* Columns' missing values as fields(), per attribute; once asked. */
	bool	   *missing_known;
	bool	   *missing_null;
	StringInfoData *missing_bytes;
	bool		enums_mapped;	/* enum labels decoded through a mapping */
};

bool
prolly_canonical_map_enums(ProllyValueEncoder *enc, List *mappings)
{
	bool		changed = false;

	for (int i = 0; i < enc->desc->natts; i++)
		if (enc->codecs[i])
			changed |= prolly_datum_map_enums(enc->codecs[i], mappings);
	if (changed)
		enc->enums_mapped = true;
	return changed;
}

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
	enc->typdefs = MemoryContextAlloc(mcxt, sizeof(uint64) * Max(natts, 1));
	enc->typdef_known = MemoryContextAllocZero(mcxt, sizeof(bool) * Max(natts, 1));
	enc->missing_known = MemoryContextAllocZero(mcxt, sizeof(bool) * Max(natts, 1));
	enc->missing_null = MemoryContextAllocZero(mcxt, sizeof(bool) * Max(natts, 1));
	enc->missing_bytes = MemoryContextAllocZero(mcxt, sizeof(StringInfoData) * Max(natts, 1));
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

/* Attribute i's type's definition (prolly_datum_definition()), hashed. */
static uint64
column_typdef(ProllyValueEncoder *enc, int i)
{
	if (!enc->typdef_known[i])
	{
		StringInfoData def;

		initStringInfo(&def);
		prolly_datum_definition(TupleDescAttr(enc->desc, i)->atttypid, &def);
		enc->typdefs[i] = hash_bytes_extended((const unsigned char *) def.data,
											  def.len, 0);
		enc->typdef_known[i] = true;
		pfree(def.data);
	}
	return enc->typdefs[i];
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

static ProllyValue *row_refs_by_column(ProllyValueEncoder *enc,
									   const ProllyValue *row, int *n);

/*
 * Encode slot's row into out; with before, each column marked in kept is
 * before's reference there, slot's value not read.
 */
static void
encode_into(ProllyValueEncoder *enc, TupleTableSlot *slot,
			const ProllyValue *before, const bool *kept, StringInfo out)
{
	MemoryContext old;
	int			n = 0;
	ProllyValue *brefs = NULL;
	int			nb = 0;

	if (enc->decode_only)
		elog(ERROR, "a prolly encoder for an earlier row layout can only decode");
	slot_getallattrs(slot);

	/* Columns up to the last that is non-NULL or has a missing value. */
	for (int j = 0; j < enc->nlive; j++)
	{
		int			i = enc->live_attno[j];

		enc->nulls[j] = slot->tts_isnull[i] && !(before && kept[i]);
		if (!enc->nulls[j] || enc->has_missing[j])
			n = j + 1;
	}

	old = MemoryContextSwitchTo(enc->tmpcxt);
	if (before)
		brefs = row_refs_by_column(enc, before, &nb);
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

		/* A value kept: the reference before holds, not read. */
		if (before && kept[i])
		{
			if (j >= nb || brefs[j].len == 0)
				elog(ERROR, "a kept prolly column has no reference in the row it is kept from");
			prolly_put_varint(out, VALUE_INLINE_MAX + 1 + (uint64) brefs[j].len);
			appendBinaryStringInfo(out, (const char *) brefs[j].data,
								   brefs[j].len);
			continue;
		}

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
							att->atttypid, column_typdef(enc, i), &ref))
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
							att->atttypid, column_typdef(enc, i), &r);
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

static void
canonical_encode(ProllyValueEncoder *enc, TupleTableSlot *slot, StringInfo out)
{
	encode_into(enc, slot, NULL, NULL, out);
}

static void
canonical_encode_kept(ProllyValueEncoder *enc, TupleTableSlot *slot,
					  const ProllyValue *before, const bool *kept,
					  StringInfo out)
{
	encode_into(enc, slot, before, kept, out);
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
 * The value of attribute i stored as len bytes at p (out_of_line: a
 * reference to its value tree), in the current memory context.
 */
static Datum
decode_value(ProllyValueEncoder *enc, int i, const char *p, uint64 len,
			 bool out_of_line)
{
	ProllyValue ref = {(const uint8 *) p, (uint32) len};
	ProllyHash	root;
	uint64		vlen;
	varlena    *pointer;
	Datum		d;

	pgstat_count_prolly(PGSTAT_PROLLY_COLUMNS_DECODED, 1);
	if (!out_of_line)
		return prolly_datum_decode(enc->codecs[i], p, (int) len);
	if (enc->values == NULL)
		no_values();

	/* a tree a hash pointer can name: named, not read */
	pointer = NULL;
	if (enc->values->pointers)
	{
		PROLLY_CALL(enc->values->tree, value_root, &ref, &root, &vlen);
		pointer = prolly_toast_pointer(&root, vlen,
									   TupleDescAttr(enc->desc, i)->atttypid);
	}
	if (pointer != NULL)
		return PointerGetDatum(pointer);

	INJECTION_POINT("prolly-value-read", NULL);
	resetStringInfo(&enc->scratch);
	PROLLY_CALL(enc->values->tree, value_read, enc->values->env,
				&ref, 0, PG_UINT64_MAX, &enc->scratch);
	pgstat_count_prolly(PGSTAT_PROLLY_VALUES_READ, 1);
	pgstat_count_prolly(PGSTAT_PROLLY_VALUE_BYTES_READ, enc->scratch.len);
	d = prolly_datum_decode(enc->codecs[i], enc->scratch.data,
							enc->scratch.len);
	if (enc->scratch.maxlen > 64 * 1024)
	{
		/* (a long value's buffer isn't kept for the rows after it) */
		MemoryContext cxt = MemoryContextSwitchTo(GetMemoryChunkContext(enc));

		pfree(enc->scratch.data);
		initStringInfo(&enc->scratch);
		MemoryContextSwitchTo(cxt);
	}
	return d;
}

/*
 * Decode value into slot, only the attributes wanted marks (NULL: all);
 * with before, each column whose value is out of line with the same
 * reference in before is left NULL, unread, and marked in kept.
 */
static void
decode_into(ProllyValueEncoder *enc, const ProllyValue *value,
			const ProllyValue *before, const bool *wanted,
			TupleTableSlot *slot, bool *kept)
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
		bool		want = (wanted == NULL || wanted[i]);

		if (j >= n)
		{
			/* Absent: the row predates the column. */
			if (enc->has_missing[j] && want)
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

			/*
			 * No codec: the column was dropped after the row was encoded.
			 * Not wanted: skipped by its length, neither decoded nor read.
			 */
			if (enc->codecs[i] == NULL || !want)
				;
			else if (out_of_line && j < nb &&
					 brefs[j].len == len && memcmp(brefs[j].data, p, len) == 0)
				kept[i] = true;
			else
			{
				slot->tts_values[i] = decode_value(enc, i, p, len, out_of_line);
				slot->tts_isnull[i] = false;
			}
			p += len;
		}
	}

	/* Columns added after the rows' layout. */
	for (int x = 0; x < enc->nextra; x++)
	{
		int			i = enc->extra_attno[x];

		if (TupleDescAttr(desc, i)->atthasmissing &&
			(wanted == NULL || wanted[i]))
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
canonical_decode(ProllyValueEncoder *enc, const ProllyValue *value,
				 TupleTableSlot *slot)
{
	decode_into(enc, value, NULL, NULL, slot, NULL);
}

static void
canonical_decode_kept(ProllyValueEncoder *enc, const ProllyValue *value,
					  const ProllyValue *before, TupleTableSlot *slot,
					  bool *kept)
{
	decode_into(enc, value, before, NULL, slot, kept);
}

static void
canonical_decode_columns(ProllyValueEncoder *enc, const ProllyValue *value,
						 const bool *wanted, TupleTableSlot *slot)
{
	decode_into(enc, value, NULL, wanted, slot, NULL);
}

/*
 * Attribute i's missing value as a field: its nullness and canonical bytes,
 * encoded once per encoder.  Its bytes are held inline whatever their
 * length (a reference would mean writing a value tree).
 */
static void
missing_field(ProllyValueEncoder *enc, int i, ProllyEncodedField *f)
{
	f->stored = false;
	f->missing = true;
	f->out_of_line = false;
	if (!enc->missing_known[i])
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(enc));
		bool		isnull;
		Datum		d = getmissingattr(enc->desc, i + 1, &isnull);

		enc->missing_null[i] = isnull;
		initStringInfo(&enc->missing_bytes[i]);
		if (!isnull)
			prolly_datum_encode(enc->codecs[i], d, &enc->missing_bytes[i]);
		enc->missing_known[i] = true;
		MemoryContextSwitchTo(old);
	}
	f->isnull = enc->missing_null[i];
	f->bytes.data = f->isnull ? NULL : (const uint8 *) enc->missing_bytes[i].data;
	f->bytes.len = f->isnull ? 0 : enc->missing_bytes[i].len;
}

/*
 * The fields of an encoded row, by attribute index: one pass over its
 * header and length prefixes, checked as decode_into() checks them.  A
 * column the row predates is its missing value.
 */
static void
canonical_fields(ProllyValueEncoder *enc, const ProllyValue *row,
				 ProllyEncodedField *fields)
{
	const char *p = (const char *) row->data;
	const char *end = p + row->len;
	const char *nulls;
	int			n;

	for (int i = 0; i < enc->desc->natts; i++)
	{
		fields[i].stored = false;
		fields[i].missing = false;
		fields[i].isnull = true;
		fields[i].out_of_line = false;
		fields[i].bytes.data = NULL;
		fields[i].bytes.len = 0;
	}
	n = (int) prolly_get_varint(&p, end);
	if (n > enc->nlive || end - p < (n + 7) / 8)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row header")));
	nulls = p;
	p += (n + 7) / 8;
	for (int j = 0; j < n; j++)
	{
		int			i = enc->live_attno[j];
		uint64		len;
		bool		out_of_line;

		if (((uint8) nulls[j / 8] >> (j % 8)) & 1)
		{
			if (enc->codecs[i] != NULL)
				fields[i].stored = true;
			continue;
		}
		len = prolly_get_varint(&p, end);
		out_of_line = len > VALUE_INLINE_MAX;
		if (out_of_line)
			len -= VALUE_INLINE_MAX + 1;
		if (len > (uint64) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid canonical prolly row: value runs past the end")));
		/* (a column dropped since the row was encoded isn't the row's now) */
		if (enc->codecs[i] != NULL)
		{
			fields[i].stored = true;
			fields[i].isnull = false;
			fields[i].out_of_line = out_of_line;
			fields[i].bytes.data = (const uint8 *) p;
			fields[i].bytes.len = (uint32) len;
		}
		p += len;
	}
	if (p != end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid canonical prolly row: %d trailing bytes",
						(int) (end - p))));

	/* Columns the row predates: absent from it, or added after its layout. */
	for (int j = n; j < enc->nlive; j++)
		if (enc->has_missing[j])
			missing_field(enc, enc->live_attno[j], &fields[enc->live_attno[j]]);
	for (int x = 0; x < enc->nextra; x++)
	{
		int			i = enc->extra_attno[x];

		if (TupleDescAttr(enc->desc, i)->atthasmissing)
			missing_field(enc, i, &fields[i]);
	}
}

static Datum
canonical_decode_field(ProllyValueEncoder *enc, int attno,
					   const ProllyEncodedField *field, bool *isnull)
{
	Form_pg_attribute att;

	if (attno < 0 || attno >= enc->desc->natts)
		elog(ERROR, "attribute index %d is not one of a prolly row's", attno);
	att = TupleDescAttr(enc->desc, attno);
	*isnull = true;
	if (att->attisdropped || enc->codecs[attno] == NULL)
		return (Datum) 0;
	if (field->missing)
		return getmissingattr(enc->desc, attno + 1, isnull);
	if (!field->stored || field->isnull)
		return (Datum) 0;
	*isnull = false;
	return decode_value(enc, attno, (const char *) field->bytes.data,
						field->bytes.len, field->out_of_line);
}

/*
 * Whether two fields of attribute attno hold the same value: both NULL, or
 * the same canonical bytes.  A value is inline exactly when its bytes are
 * VALUE_INLINE_MAX or shorter, so an inline value and a reference differ,
 * but for a long missing value (held inline): that is compared with the
 * reference's value, read.
 */
static bool
canonical_fields_alike(ProllyValueEncoder *enc, int attno,
					   const ProllyEncodedField *a,
					   const ProllyEncodedField *b)
{
	const ProllyEncodedField *inl;
	const ProllyEncodedField *ref;
	ProllyValue r;
	ProllyHash	root;
	uint64		vlen;
	bool		same;

	if (attno < 0 || attno >= enc->desc->natts)
		elog(ERROR, "attribute index %d is not one of a prolly row's", attno);
	if (a->isnull || b->isnull)
		return a->isnull && b->isnull;
	if (a->out_of_line == b->out_of_line)
		return a->bytes.len == b->bytes.len &&
			memcmp(a->bytes.data, b->bytes.data, a->bytes.len) == 0;
	inl = a->out_of_line ? b : a;
	ref = a->out_of_line ? a : b;
	if (inl->bytes.len <= VALUE_INLINE_MAX)
		return false;
	if (enc->values == NULL)
		no_values();
	r = ref->bytes;
	PROLLY_CALL(enc->values->tree, value_root, &r, &root, &vlen);
	if (vlen != inl->bytes.len)
		return false;
	resetStringInfo(&enc->scratch);
	PROLLY_CALL(enc->values->tree, value_read, enc->values->env,
				&r, 0, PG_UINT64_MAX, &enc->scratch);
	pgstat_count_prolly(PGSTAT_PROLLY_VALUES_READ, 1);
	pgstat_count_prolly(PGSTAT_PROLLY_VALUE_BYTES_READ, enc->scratch.len);
	same = enc->scratch.len == (int) inl->bytes.len &&
		memcmp(enc->scratch.data, inl->bytes.data, inl->bytes.len) == 0;
	return same;
}

/*
 * Whether attribute attno of enc's rows and other_attno of other's encode
 * values alike: equal bytes the same value and unequal bytes different
 * ones.  Both live, of the same type and typmod, the type defined alike
 * (prolly_datum_definition()), and neither encoder decoding enum labels
 * through a historical mapping.
 */
static bool
canonical_field_compatible(ProllyValueEncoder *enc, int attno,
						   ProllyValueEncoder *other, int other_attno)
{
	Form_pg_attribute a;
	Form_pg_attribute b;

	if (attno < 0 || attno >= enc->desc->natts ||
		other_attno < 0 || other_attno >= other->desc->natts)
		return false;
	a = TupleDescAttr(enc->desc, attno);
	b = TupleDescAttr(other->desc, other_attno);
	if (a->attisdropped || b->attisdropped ||
		enc->codecs[attno] == NULL || other->codecs[other_attno] == NULL)
		return false;
	if (a->atttypid != b->atttypid || a->atttypmod != b->atttypmod)
		return false;
	if (enc->enums_mapped || other->enums_mapped)
		return false;
	return column_typdef(enc, attno) == column_typdef(other, other_attno);
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
	for (int i = 0; i < enc->desc->natts; i++)
		if (enc->missing_known[i])
			pfree(enc->missing_bytes[i].data);
	pfree(enc->missing_known);
	pfree(enc->missing_null);
	pfree(enc->missing_bytes);
	pfree(enc);
}

const ProllyValueEncoderRoutine prolly_valenc_canonical = {
	.info = {
		.iface = PROLLY_IFACE_VALUE_ENCODER,
		.impl_id = 2,
		/* (pinned by canon_folded.c: change FOLDED_FORMAT with it) */
		.format_version = 6,		/* 5: tagged values; 6: extension send round trips */
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
	.encode_kept = canonical_encode_kept,
	.fields = canonical_fields,
	.decode_field = canonical_decode_field,
	.fields_alike = canonical_fields_alike,
	.field_compatible = canonical_field_compatible,
	.decode_columns = canonical_decode_columns,
};
