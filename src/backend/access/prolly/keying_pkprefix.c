/*-------------------------------------------------------------------------
 *
 * keying_pkprefix.c
 *	  Table keying implementation "pk_or_prefix": keyed by the primary
 *	  key if the table has one, else by row content's first bytes and hash.
 *
 * A table with a primary key (checked immediately; a deferrable one may
 * hold duplicates until commit, so its table is keyed like one without):
 *
 *	key		the primary key columns, through the key encoder, in the
 *			order, opfamilies, collations and options of the key's index
 *	value	the row, through the value encoder
 *
 * A table without one (as in Dolt's keyless tables):
 *
 *	key		the first KEY_PREFIX bytes of the encoded row's values (after
 *			its column count and NULL bitmap; all of them if shorter),
 *			then its content hash
 *	value	varint count of identical rows, then the row
 *
 * so identical rows are one item, and the tree depends only on which rows
 * the table holds, not on the order they were inserted in.  A row's
 * content hash is the content-hash implementation's digest of its encoded
 * row.  The prefix keeps rows that begin alike (on their first columns)
 * together in the tree, so a change to a run of them changes a few chunks,
 * not one each; the hash makes the key the row's alone.  Keys are compared
 * as bytes, so the order depends on no collation.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/keying_pkprefix.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "utils/rel.h"

/* How much of a row without a primary key begins its key. */
#define KEY_PREFIX	32

struct ProllyKeying
{
	MemoryContext mcxt;
	const ProllyHashRoutine *hash;
	const ProllyValueEncoderRoutine *valenc_routine;
	ProllyValueEncoder *valenc;
	TupleDesc	desc;			/* the relation's, when opened */
	StringInfoData layout;		/* valenc's row layout */

	/* Decoding into slots whose column types differ from desc's. */
	TupleDesc	alt_desc;
	ProllyValueEncoder *alt_valenc;

	const ProllyValueStore *values; /* values out of line; NULL if none */

	/* Primary key, if any. */
	bool		keyless;
	const ProllyKeyEncoderRoutine *keyenc_routine;
	ProllyKeyEncoder *keyenc;
	int			nkeys;
	AttrNumber *key_attnos;		/* table attribute numbers of key columns */
	Oid		   *opfamilies;
	Oid		   *collations;
	int16	   *options;
	Oid		   *types;
	Datum	   *key_values;		/* scratch */
	bool	   *key_isnull;
};

typedef struct KeyColumn
{
	int16		attno;
	int16		option;
	Oid			opfamily;
	Oid			collation;
	Oid			type;
} KeyColumn;

static void
hash_row(ProllyKeying *k, const uint8 *data, Size len, ProllyHash *out)
{
	ProllyHashCtx *ctx = PROLLY_CALL(k->hash, begin, CurrentMemoryContext);

	PROLLY_CALL(k->hash, update, ctx, data, len);
	PROLLY_CALL(k->hash, finish, ctx, out);
}

/* Set up encoding of a primary key on the given columns. */
static void
setup_key(ProllyKeying *k, Relation rel, int nkeys, const KeyColumn *cols,
		  const ProllyKeyEncoderRoutine *keyenc)
{
	ProllyKeyDesc desc;

	k->nkeys = nkeys;
	k->key_attnos = MemoryContextAlloc(k->mcxt, sizeof(AttrNumber) * nkeys);
	k->key_values = MemoryContextAlloc(k->mcxt, sizeof(Datum) * nkeys);
	k->key_isnull = MemoryContextAlloc(k->mcxt, sizeof(bool) * nkeys);
	k->opfamilies = MemoryContextAlloc(k->mcxt, sizeof(Oid) * nkeys);
	k->collations = MemoryContextAlloc(k->mcxt, sizeof(Oid) * nkeys);
	k->options = MemoryContextAlloc(k->mcxt, sizeof(int16) * nkeys);
	k->types = MemoryContextAlloc(k->mcxt, sizeof(Oid) * nkeys);
	for (int i = 0; i < nkeys; i++)
	{
		/* Primary keys are plain columns, never expressions. */
		if (cols[i].attno <= 0 || cols[i].attno > RelationGetNumberOfAttributes(rel))
			elog(ERROR, "primary key of \"%s\" has an invalid key column",
				 RelationGetRelationName(rel));
		k->key_attnos[i] = cols[i].attno;
		k->opfamilies[i] = cols[i].opfamily;
		k->collations[i] = cols[i].collation;
		k->options[i] = cols[i].option;
		k->types[i] = cols[i].type;
	}
	k->keyenc_routine = keyenc;
	desc.nkeys = nkeys;
	desc.types = k->types;
	desc.opfamilies = k->opfamilies;
	desc.collations = k->collations;
	desc.options = k->options;
	k->keyenc = PROLLY_CALL(keyenc, begin, &desc, k->mcxt);
}

/* The key columns of rel's primary key index pkoid. */
static int
primary_key_columns(Relation rel, Oid pkoid, KeyColumn *cols)
{
	Relation	idx = index_open(pkoid, AccessShareLock);
	int			nkeys = IndexRelationGetNumberOfKeyAttributes(idx);

	for (int i = 0; i < nkeys; i++)
	{
		memset(&cols[i], 0, sizeof(KeyColumn));
		cols[i].attno = idx->rd_index->indkey.values[i];
		cols[i].opfamily = idx->rd_opfamily[i];
		cols[i].collation = idx->rd_indcollation[i];
		cols[i].option = idx->rd_indoption[i];
		if (cols[i].attno > 0 && cols[i].attno <= RelationGetNumberOfAttributes(rel))
			cols[i].type = TupleDescAttr(RelationGetDescr(rel),
										 cols[i].attno - 1)->atttypid;
	}
	index_close(idx, AccessShareLock);
	return nkeys;
}

/*
 * Rows of rel as keyed now, encoded with the table's columns as they are
 * once its missing values are cleared (k->desc is a copy without them).
 * Everything this allocates, temporaries included, is in mcxt: a keying is
 * often kept longer than its caller's memory (a cache), so nothing it keeps
 * may be in the caller's.
 */
static ProllyKeying *
pkprefix_open(Relation rel, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ProllyComponents c;
	ProllyKeying *k = palloc0(sizeof(ProllyKeying));
	KeyColumn	cols[INDEX_MAX_KEYS];
	int			nkeys;

	/* Not a deferrable one: it may hold duplicates until commit. */
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);

	prolly_components_for_relation(rel, &c);
	k->mcxt = mcxt;
	k->hash = c.hash;
	k->valenc_routine = c.value_encoder;
	k->desc = CreateTupleDescCopy(RelationGetDescr(rel));
	k->valenc = PROLLY_CALL(c.value_encoder, begin, k->desc, mcxt);

	k->keyless = !OidIsValid(pkoid);
	nkeys = k->keyless ? 0 : primary_key_columns(rel, pkoid, cols);

	/*
	 * A primary key the key encoder can't order (WITHOUT OVERLAPS, kept by
	 * a GiST index) doesn't key the rows: they are keyed by content.
	 */
	for (int i = 0; i < nkeys && !k->keyless; i++)
		if (!PROLLY_CALL(c.key_encoder, supports, cols[i].opfamily,
						 cols[i].type, cols[i].collation))
			k->keyless = true;
	if (!k->keyless)
		setup_key(k, rel, nkeys, cols, c.key_encoder);
	initStringInfo(&k->layout);
	PROLLY_CALL(c.value_encoder, layout, k->valenc, &k->layout);
	MemoryContextSwitchTo(old);
	return k;
}


/*
 * Rows of desc's columns, keyed by content, opened without a relation, as
 * pkprefix_open() opens a table without a primary key.
 */
static ProllyKeying *
pkprefix_open_described(TupleDesc desc, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ProllyComponents c;
	ProllyKeying *k = palloc0(sizeof(ProllyKeying));

	prolly_history_components(&c);
	k->mcxt = mcxt;
	k->hash = c.hash;
	k->valenc_routine = c.value_encoder;
	k->desc = CreateTupleDescCopy(desc);
	k->valenc = PROLLY_CALL(c.value_encoder, begin, k->desc, mcxt);
	k->keyless = true;
	initStringInfo(&k->layout);
	PROLLY_CALL(c.value_encoder, layout, k->valenc, &k->layout);
	MemoryContextSwitchTo(old);
	return k;
}

static bool
pkprefix_keyless(ProllyKeying *k)
{
	return k->keyless;
}

/* Append the primary key of slot's row to key. */
static void
encode_pk(ProllyKeying *k, TupleTableSlot *slot, StringInfo key)
{
	slot_getallattrs(slot);
	for (int i = 0; i < k->nkeys; i++)
	{
		int			att = k->key_attnos[i] - 1;

		k->key_values[i] = slot->tts_values[att];
		k->key_isnull[i] = slot->tts_isnull[att];
	}
	PROLLY_CALL(k->keyenc_routine, encode, k->keyenc, k->key_values,
				k->key_isnull, key);
}

static void
pkprefix_encode_row(ProllyKeying *k, TupleTableSlot *slot, StringInfo key,
					 StringInfo row, ProllyHash *content)
{
	int			start = row->len;

	PROLLY_CALL(k->valenc_routine, encode, k->valenc, slot, row);
	hash_row(k, (const uint8 *) row->data + start, row->len - start, content);

	if (k->keyless)
	{
		/*
		 * (from after the row's header, its column count and NULLs, so rows
		 * that begin alike stay together whatever their later columns'
		 * NULLs)
		 */
		const char *p = row->data + start;
		const char *end = row->data + row->len;
		uint64		n = prolly_get_varint(&p, end);

		p += Min((uint64) (end - p), (n + 7) / 8);
		appendBinaryStringInfo(key, p, Min(end - p, KEY_PREFIX));
		appendBinaryStringInfo(key, content->bytes, content->len);
		return;
	}

	encode_pk(k, slot, key);
}

static bool
pkprefix_encode_key(ProllyKeying *k, TupleTableSlot *slot, StringInfo key)
{
	if (k->keyless)
		return false;
	encode_pk(k, slot, key);
	return true;
}

static void
pkprefix_make_value(ProllyKeying *k, const ProllyValue *row, uint64 count,
					 StringInfo value)
{
	if (k->keyless)
	{
		Assert(count >= 1);
		prolly_put_varint(value, count);
	}
	else if (count != 1)
		elog(ERROR, "a table keyed by its primary key holds one row per key");
	appendBinaryStringInfo(value, (const char *) row->data, row->len);
}

static void
pkprefix_parse_value_as(ProllyKeying *k, bool keyless, const ProllyValue *value,
						ProllyValue *row, uint64 *count)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

	if (keyless)
	{
		*count = prolly_get_varint(&p, end);
		if (*count == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("keyless prolly row has a count of zero")));
	}
	else
		*count = 1;
	row->data = (const uint8 *) p;
	row->len = end - p;
}

static void
pkprefix_parse_value(ProllyKeying *k, const ProllyValue *value,
					  ProllyValue *row, uint64 *count)
{
	pkprefix_parse_value_as(k, k->keyless, value, row, count);
}

/* The encoder that decodes into slot's descriptor. */
static ProllyValueEncoder *
decoder_for(ProllyKeying *k, TupleTableSlot *slot)
{
	TupleDesc	sd = slot->tts_tupleDescriptor;
	ProllyValueEncoder *enc = k->valenc;
	bool		same = (sd->natts == k->desc->natts);

	/*
	 * A slot may give columns the types they had when the rows were written
	 * (a table rewrite reads the old rows that way).
	 */
	for (int i = 0; same && i < sd->natts; i++)
		same = TupleDescAttr(sd, i)->atttypid == TupleDescAttr(k->desc, i)->atttypid &&
			TupleDescAttr(sd, i)->atttypmod == TupleDescAttr(k->desc, i)->atttypmod;
	if (!same)
	{
		if (k->alt_desc != sd)
		{
			ProllyValue layout = {(const uint8 *) k->layout.data, k->layout.len};

			if (k->alt_valenc)
				PROLLY_CALL(k->valenc_routine, end, k->alt_valenc);
			k->alt_valenc = PROLLY_CALL(k->valenc_routine, begin_layout, sd,
										&layout, k->mcxt);
			if (k->values)
				PROLLY_CALL(k->valenc_routine, set_values, k->alt_valenc,
							k->values);
			k->alt_desc = sd;
		}
		enc = k->alt_valenc;
	}
	return enc;
}

static void
pkprefix_decode_row(ProllyKeying *k, const ProllyValue *row,
					 TupleTableSlot *slot)
{
	PROLLY_CALL(k->valenc_routine, decode, decoder_for(k, slot), row, slot);
}

static void
pkprefix_decode_row_kept(ProllyKeying *k, const ProllyValue *row,
						 const ProllyValue *before, TupleTableSlot *slot,
						 bool *kept)
{
	PROLLY_CALL(k->valenc_routine, decode_kept, decoder_for(k, slot), row,
				before, slot, kept);
}

static int
pkprefix_compare(ProllyKeying *k, const ProllyKey *a, const ProllyKey *b)
{
	int			r;

	if (!k->keyless)
		return PROLLY_CALL(k->keyenc_routine, compare, k->keyenc, a, b);
	r = memcmp(a->data, b->data, Min(a->len, b->len));
	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

static void
pkprefix_set_values(ProllyKeying *k, const ProllyValueStore *values)
{
	k->values = values;
	PROLLY_CALL(k->valenc_routine, set_values, k->valenc, values);
	if (k->alt_valenc)
		PROLLY_CALL(k->valenc_routine, set_values, k->alt_valenc, values);
}

static void
pkprefix_row_refs(ProllyKeying *k, const ProllyValue *row, StringInfo refs)
{
	PROLLY_CALL(k->valenc_routine, refs, k->valenc, row, refs);
}

static void
pkprefix_close(ProllyKeying *k)
{
	PROLLY_CALL(k->valenc_routine, end, k->valenc);
	if (k->alt_valenc)
		PROLLY_CALL(k->valenc_routine, end, k->alt_valenc);
	if (k->keyenc)
		PROLLY_CALL(k->keyenc_routine, end, k->keyenc);
	FreeTupleDesc(k->desc);
	pfree(k->layout.data);
	pfree(k);
}

const ProllyKeyingRoutine prolly_keying_pk_or_prefix = {
	.info = {
		.iface = PROLLY_IFACE_TABLE_KEYING,
		.impl_id = 2,
		.format_version = 1,
		.name = "pk_or_prefix",
		.description = "keyed by the primary key, or by row content's first "
		"bytes and hash with a count for tables without one",
	},
	.open = pkprefix_open,
	.keyless = pkprefix_keyless,
	.encode_row = pkprefix_encode_row,
	.make_value = pkprefix_make_value,
	.parse_value = pkprefix_parse_value,
	.parse_value_as = pkprefix_parse_value_as,
	.open_described = pkprefix_open_described,
	.decode_row = pkprefix_decode_row,
	.compare = pkprefix_compare,
	.close = pkprefix_close,
	.set_values = pkprefix_set_values,
	.row_refs = pkprefix_row_refs,
	.decode_row_kept = pkprefix_decode_row_kept,
	.encode_key = pkprefix_encode_key,
};
