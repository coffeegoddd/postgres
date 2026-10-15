/*-------------------------------------------------------------------------
 *
 * keying_pkcontent.c
 *	  Table keying implementation "pk_or_content": keyed by the primary
 *	  key if the table has one, else by row content.
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
 *	key		the row's content hash
 *	value	varint count of identical rows, then the row
 *
 * so identical rows are one item, and the tree depends only on which rows
 * the table holds, not on the order they were inserted in.  A row's
 * content hash is the content-hash implementation's digest of its encoded
 * row.
 *
 * A description is 'C' for content keying or 'P' for a primary key, the
 * length and bytes of the value encoder's row layout, and for 'P' the
 * number of key columns and per column its attribute number, options,
 * opfamily, collation and type, in native byte order (descriptions are
 * local).  The type is recorded because a table's old rows must stay
 * readable after their key column is dropped, when the relation no longer
 * says what type it had.
 *
 * The key encoder starts the first time keys are encoded or compared: a
 * scan of old rows needs neither, and the key's type may be gone by then
 * (DROP TYPE ... CASCADE drops the column with it).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/keying_pkcontent.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "utils/rel.h"
#include "utils/syscache.h"

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
}

static ProllyKeyEncoder *
key_encoder(ProllyKeying *k)
{
	ProllyKeyDesc desc;

	if (k->keyenc)
		return k->keyenc;
	desc.nkeys = k->nkeys;
	desc.types = k->types;
	desc.opfamilies = k->opfamilies;
	desc.collations = k->collations;
	desc.options = k->options;
	k->keyenc = PROLLY_CALL(k->keyenc_routine, begin, &desc, k->mcxt);
	return k->keyenc;
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

/* A parsed description; layout points into it. */
typedef struct Description
{
	bool		keyless;
	ProllyValue layout;
	int			nkeys;
	KeyColumn	cols[INDEX_MAX_KEYS];
} Description;

static void
parse_description(const ProllyValue *d, Description *out)
{
	Size		off = 0;
	uint16		len;
	uint16		nkeys;

	if (d->len < 1 + sizeof(uint16) || (d->data[0] != 'C' && d->data[0] != 'P'))
		goto bad;
	out->keyless = (d->data[0] == 'C');
	off = 1;
	memcpy(&len, d->data + off, sizeof(len));
	off += sizeof(len);
	if (d->len < off + len)
		goto bad;
	out->layout.data = d->data + off;
	out->layout.len = len;
	off += len;
	out->nkeys = 0;
	if (out->keyless)
	{
		if (off != d->len)
			goto bad;
		return;
	}
	if (d->len < off + sizeof(uint16))
		goto bad;
	memcpy(&nkeys, d->data + off, sizeof(nkeys));
	off += sizeof(nkeys);
	if (nkeys == 0 || nkeys > INDEX_MAX_KEYS ||
		d->len != off + nkeys * sizeof(KeyColumn))
		goto bad;
	memcpy(out->cols, d->data + off, nkeys * sizeof(KeyColumn));
	out->nkeys = nkeys;
	return;

bad:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid prolly table keying description")));
}

/*
 * Everything open() allocates, temporaries included, is in mcxt: a keying
 * is often kept longer than its caller's memory (a cache), so nothing it
 * keeps may be in the caller's.
 */
/*
 * rewritten: encode with the table's columns as they are once its missing
 * values are cleared (k->desc is a copy without them).
 */
static ProllyKeying *
open_keying(Relation rel, const ProllyValue *description, bool rewritten,
			MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ProllyComponents c;
	ProllyKeying *k = palloc0(sizeof(ProllyKeying));
	Description d;

	prolly_components_for_relation(rel, &c);
	k->mcxt = mcxt;
	k->hash = c.hash;
	k->valenc_routine = c.value_encoder;
	k->desc = CreateTupleDescCopy(RelationGetDescr(rel));

	if (description)
	{
		parse_description(description, &d);
		k->valenc = PROLLY_CALL(c.value_encoder, begin_layout,
								RelationGetDescr(rel), &d.layout, mcxt);
	}
	else
	{
		/* Not a deferrable one: it may hold duplicates until commit. */
		Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);

		d.keyless = !OidIsValid(pkoid);
		d.nkeys = d.keyless ? 0 : primary_key_columns(rel, pkoid, d.cols);

		/*
		 * A primary key the key encoder can't order (WITHOUT OVERLAPS, kept
		 * by a GiST index) doesn't key the rows: they are keyed by content.
		 */
		for (int i = 0; i < d.nkeys && !d.keyless; i++)
			if (!PROLLY_CALL(c.key_encoder, supports, d.cols[i].opfamily,
							 d.cols[i].type, d.cols[i].collation))
				d.keyless = true;
		if (d.keyless)
			d.nkeys = 0;
		k->valenc = PROLLY_CALL(c.value_encoder, begin,
								rewritten ? k->desc : RelationGetDescr(rel),
								mcxt);
	}
	k->keyless = d.keyless;
	if (!k->keyless)
		setup_key(k, rel, d.nkeys, d.cols, c.key_encoder);
	initStringInfo(&k->layout);
	PROLLY_CALL(c.value_encoder, layout, k->valenc, &k->layout);
	MemoryContextSwitchTo(old);
	return k;
}

static ProllyKeying *
pkcontent_open(Relation rel, const ProllyValue *description, MemoryContext mcxt)
{
	return open_keying(rel, description, false, mcxt);
}

static ProllyKeying *
pkcontent_open_rewritten(Relation rel, MemoryContext mcxt)
{
	return open_keying(rel, NULL, true, mcxt);
}

static void
pkcontent_describe(ProllyKeying *k, StringInfo out)
{
	uint16		nkeys = (uint16) k->nkeys;
	StringInfoData layout;
	uint16		len;

	initStringInfo(&layout);
	PROLLY_CALL(k->valenc_routine, layout, k->valenc, &layout);
	len = (uint16) layout.len;
	appendStringInfoChar(out, k->keyless ? 'C' : 'P');
	appendBinaryStringInfo(out, (const char *) &len, sizeof(len));
	appendBinaryStringInfo(out, layout.data, layout.len);
	pfree(layout.data);
	if (k->keyless)
		return;
	appendBinaryStringInfo(out, (const char *) &nkeys, sizeof(nkeys));
	for (int i = 0; i < k->nkeys; i++)
	{
		KeyColumn	col;

		memset(&col, 0, sizeof(col));
		col.attno = k->key_attnos[i];
		col.option = k->options[i];
		col.opfamily = k->opfamilies[i];
		col.collation = k->collations[i];
		col.type = k->types[i];
		appendBinaryStringInfo(out, (const char *) &col, sizeof(col));
	}
}

/*
 * Rows read correctly if they are keyed the same way and the value
 * encoder reads their layout.
 */
static bool
pkcontent_can_read(ProllyKeying *k, const ProllyValue *description)
{
	Description d;

	parse_description(description, &d);
	if (d.keyless != k->keyless || d.nkeys != k->nkeys)
		return false;
	for (int i = 0; i < d.nkeys; i++)
	{
		if (d.cols[i].attno != k->key_attnos[i] ||
			d.cols[i].opfamily != k->opfamilies[i] ||
			d.cols[i].collation != k->collations[i] ||
			d.cols[i].option != k->options[i] ||
			d.cols[i].type != k->types[i])
			return false;
	}
	return PROLLY_CALL(k->valenc_routine, reads_layout, k->valenc, &d.layout);
}

static bool
pkcontent_keyless(ProllyKeying *k)
{
	return k->keyless;
}

static void
pkcontent_encode_row(ProllyKeying *k, TupleTableSlot *slot, StringInfo key,
					 StringInfo row, ProllyHash *content)
{
	int			start = row->len;

	PROLLY_CALL(k->valenc_routine, encode, k->valenc, slot, row);
	hash_row(k, (const uint8 *) row->data + start, row->len - start, content);

	if (k->keyless)
	{
		appendBinaryStringInfo(key, content->bytes, content->len);
		return;
	}

	slot_getallattrs(slot);
	for (int i = 0; i < k->nkeys; i++)
	{
		int			att = k->key_attnos[i] - 1;

		k->key_values[i] = slot->tts_values[att];
		k->key_isnull[i] = slot->tts_isnull[att];
	}
	PROLLY_CALL(k->keyenc_routine, encode, key_encoder(k), k->key_values,
				k->key_isnull, key);
}

static void
pkcontent_make_value(ProllyKeying *k, const ProllyValue *row, uint64 count,
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
pkcontent_parse_value(ProllyKeying *k, const ProllyValue *value,
					  ProllyValue *row, uint64 *count)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

	if (k->keyless)
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
pkcontent_content_hash(ProllyKeying *k, const ProllyValue *row,
					   ProllyHash *content)
{
	hash_row(k, row->data, row->len, content);
}

static void
pkcontent_decode_row(ProllyKeying *k, const ProllyValue *row,
					 TupleTableSlot *slot)
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
			k->alt_desc = sd;
		}
		enc = k->alt_valenc;
	}
	PROLLY_CALL(k->valenc_routine, decode, enc, row, slot);
}

static int
pkcontent_compare(ProllyKeying *k, const ProllyKey *a, const ProllyKey *b)
{
	int			r;

	if (!k->keyless)
		return PROLLY_CALL(k->keyenc_routine, compare, key_encoder(k), a, b);
	r = memcmp(a->data, b->data, Min(a->len, b->len));
	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

/*
 * Keys are ordered by their columns' btree comparison functions, which
 * need the types, operator families and collations they were recorded
 * with.
 */
static bool
pkcontent_can_compare(ProllyKeying *k)
{
	if (k->keyless)
		return true;
	for (int i = 0; i < k->nkeys; i++)
	{
		if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(k->types[i])) ||
			!SearchSysCacheExists1(OPFAMILYOID, ObjectIdGetDatum(k->opfamilies[i])) ||
			(OidIsValid(k->collations[i]) &&
			 !SearchSysCacheExists1(COLLOID, ObjectIdGetDatum(k->collations[i]))))
			return false;
	}
	return true;
}

/* Content hashes are equal only as bytes; key columns, as encoded. */
static bool
pkcontent_equal_bytes(ProllyKeying *k)
{
	if (k->keyless)
		return true;
	if (!pkcontent_can_compare(k))
		return false;
	return PROLLY_CALL(k->keyenc_routine, equal_bytes, key_encoder(k));
}

static void
pkcontent_close(ProllyKeying *k)
{
	PROLLY_CALL(k->valenc_routine, end, k->valenc);
	if (k->alt_valenc)
		PROLLY_CALL(k->valenc_routine, end, k->alt_valenc);
	if (k->keyenc)
		PROLLY_CALL(k->keyenc_routine, end, k->keyenc);
	pfree(k);
}

const ProllyKeyingRoutine prolly_keying_pk_or_content = {
	.info = {
		.iface = PROLLY_IFACE_TABLE_KEYING,
		.impl_id = 1,
		.format_version = 2,
		.name = "pk_or_content",
		.description = "keyed by the primary key, or by row content hash with "
		"a count for tables without one",
	},
	.open = pkcontent_open,
	.describe = pkcontent_describe,
	.open_rewritten = pkcontent_open_rewritten,
	.can_read = pkcontent_can_read,
	.keyless = pkcontent_keyless,
	.encode_row = pkcontent_encode_row,
	.make_value = pkcontent_make_value,
	.parse_value = pkcontent_parse_value,
	.content_hash = pkcontent_content_hash,
	.decode_row = pkcontent_decode_row,
	.compare = pkcontent_compare,
	.can_compare = pkcontent_can_compare,
	.equal_bytes = pkcontent_equal_bytes,
	.close = pkcontent_close,
};
