/*-------------------------------------------------------------------------
 *
 * ixkeying_columns.c
 *	  Index keying implementation "columns_then_row": entries are the
 *	  index columns, then the row they belong to.
 *
 * An entry is
 *
 *	key-columns row-key ordinal(8) row-key-length(4) tag(1)
 *
 * (integers big-endian, tag 0), the key columns encoded by the key encoder
 * in the index's operator families, collations and options.  A bound for
 * seeking is
 *
 *	key-columns(first n) n(2) tag(1)
 *
 * with tag 1 (before its entries) or 2 (after them).  compare() compares
 * the key columns through the key encoder (memcmp where their encodings
 * allow), then the row key and ordinal with memcmp.
 *
 * A description is 'I', the number of key columns (2 bytes), then per
 * column its type, operator family, collation and options, in native byte
 * order (descriptions are local).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/ixkeying_columns.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#define TAG_ENTRY		0
#define TAG_LOWER		1
#define TAG_UPPER		2

#define ENTRY_TRAILER	(8 + 4 + 1)
#define BOUND_TRAILER	(2 + 1)

typedef struct IxColumn
{
	Oid			type;
	Oid			opfamily;
	Oid			collation;
	int16		option;
} IxColumn;

struct ProllyIndexKeying
{
	MemoryContext mcxt;
	const ProllyKeyEncoderRoutine *keyenc;
	int			nkeys;
	IxColumn   *cols;
	Oid		   *types;
	Oid		   *opfamilies;
	Oid		   *collations;
	int16	   *options;
	ProllyKeyEncoder **prefix;	/* [k - 1]: encoder of the first k columns */
};

static ProllyKeyEncoder *
encoder(ProllyIndexKeying *k, int ncols)
{
	if (ncols < 1 || ncols > k->nkeys)
		elog(ERROR, "prolly index key prefix of %d columns out of %d",
			 ncols, k->nkeys);
	if (k->prefix[ncols - 1] == NULL)
	{
		ProllyKeyDesc desc;

		desc.nkeys = ncols;
		desc.types = k->types;
		desc.opfamilies = k->opfamilies;
		desc.collations = k->collations;
		desc.options = k->options;
		k->prefix[ncols - 1] = PROLLY_CALL(k->keyenc, begin, &desc, k->mcxt);
	}
	return k->prefix[ncols - 1];
}

static ProllyIndexKeying *
columns_open(Relation index, const ProllyValue *description, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ProllyComponents c;
	ProllyIndexKeying *k = palloc0(sizeof(ProllyIndexKeying));

	prolly_components_for_relation(index, &c);
	k->mcxt = mcxt;
	k->keyenc = c.key_encoder;
	if (description)
	{
		uint16		n;

		if (description->len < 3 || description->data[0] != 'I')
			goto bad;
		memcpy(&n, description->data + 1, sizeof(n));
		if (n == 0 || n > INDEX_MAX_KEYS ||
			description->len != 3 + n * sizeof(IxColumn))
			goto bad;
		k->nkeys = n;
		k->cols = palloc(sizeof(IxColumn) * n);
		memcpy(k->cols, description->data + 3, sizeof(IxColumn) * n);
	}
	else
	{
		k->nkeys = IndexRelationGetNumberOfKeyAttributes(index);
		k->cols = palloc0(sizeof(IxColumn) * k->nkeys);
		for (int i = 0; i < k->nkeys; i++)
		{
			/*
			 * The operator class's input type, unless that is polymorphic
			 * (anyenum, anyarray): then the indexed column's own type.
			 */
			k->cols[i].type = index->rd_opcintype[i];
			if (IsPolymorphicType(k->cols[i].type) ||
				get_typtype(k->cols[i].type) == TYPTYPE_PSEUDO)
				k->cols[i].type = TupleDescAttr(RelationGetDescr(index), i)->atttypid;
			k->cols[i].opfamily = index->rd_opfamily[i];
			k->cols[i].collation = index->rd_indcollation[i];
			k->cols[i].option = index->rd_indoption[i];
			if (!PROLLY_CALL(k->keyenc, supports, k->cols[i].opfamily,
							 k->cols[i].type, k->cols[i].collation))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("prolly indexes cannot index type %s with operator family %u",
								format_type_be(k->cols[i].type),
								k->cols[i].opfamily)));
		}
	}
	k->types = palloc(sizeof(Oid) * k->nkeys);
	k->opfamilies = palloc(sizeof(Oid) * k->nkeys);
	k->collations = palloc(sizeof(Oid) * k->nkeys);
	k->options = palloc(sizeof(int16) * k->nkeys);
	for (int i = 0; i < k->nkeys; i++)
	{
		k->types[i] = k->cols[i].type;
		k->opfamilies[i] = k->cols[i].opfamily;
		k->collations[i] = k->cols[i].collation;
		k->options[i] = k->cols[i].option;
	}
	k->prefix = palloc0(sizeof(ProllyKeyEncoder *) * k->nkeys);
	MemoryContextSwitchTo(old);
	return k;

bad:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid prolly index keying description")));
	return NULL;				/* keep compiler quiet */
}

static void
columns_describe(ProllyIndexKeying *k, StringInfo out)
{
	uint16		n = (uint16) k->nkeys;

	appendStringInfoChar(out, 'I');
	appendBinaryStringInfo(out, (const char *) &n, sizeof(n));
	for (int i = 0; i < k->nkeys; i++)
	{
		IxColumn	col;

		memset(&col, 0, sizeof(col));
		col = k->cols[i];
		appendBinaryStringInfo(out, (const char *) &col, sizeof(col));
	}
}

static int
columns_nkeys(ProllyIndexKeying *k)
{
	return k->nkeys;
}

static void
put_be(StringInfo out, uint64 v, int nbytes)
{
	for (int i = nbytes - 1; i >= 0; i--)
		appendStringInfoChar(out, (char) (v >> (i * 8)));
}

static uint64
get_be(const uint8 *p, int nbytes)
{
	uint64		v = 0;

	for (int i = 0; i < nbytes; i++)
		v = (v << 8) | p[i];
	return v;
}

static void
columns_encode_entry(ProllyIndexKeying *k, const Datum *values,
					 const bool *isnull, const ProllyKey *rowkey,
					 uint64 ordinal, StringInfo out)
{
	PROLLY_CALL(k->keyenc, encode, encoder(k, k->nkeys), values, isnull, out);
	appendBinaryStringInfo(out, (const char *) rowkey->data, rowkey->len);
	put_be(out, ordinal, 8);
	put_be(out, rowkey->len, 4);
	appendStringInfoChar(out, TAG_ENTRY);
}

static void
columns_encode_bound(ProllyIndexKeying *k, int nkeys, const Datum *values,
					 const bool *isnull, bool upper, StringInfo out)
{
	PROLLY_CALL(k->keyenc, encode, encoder(k, nkeys), values, isnull, out);
	put_be(out, nkeys, 2);
	appendStringInfoChar(out, upper ? TAG_UPPER : TAG_LOWER);
}

/* An entry or bound, split. */
typedef struct Parsed
{
	uint8		tag;
	ProllyKey	columns;		/* the encoded key columns */
	int			ncols;			/* bounds: how many */
	ProllyKey	rest;			/* entries: row key and ordinal */
	ProllyKey	rowkey;
	uint64		ordinal;
} Parsed;

static void
parse(ProllyIndexKeying *k, const ProllyKey *key, Parsed *p)
{
	if (key->len < 1)
		goto bad;
	p->tag = key->data[key->len - 1];
	if (p->tag == TAG_ENTRY)
	{
		uint32		rklen;

		if (key->len < ENTRY_TRAILER)
			goto bad;
		rklen = (uint32) get_be(key->data + key->len - 5, 4);
		if (key->len < ENTRY_TRAILER + rklen)
			goto bad;
		p->columns.data = key->data;
		p->columns.len = key->len - ENTRY_TRAILER - rklen;
		p->ncols = k->nkeys;
		p->rest.data = key->data + p->columns.len;
		p->rest.len = rklen + 8;
		p->rowkey.data = p->rest.data;
		p->rowkey.len = rklen;
		p->ordinal = get_be(p->rest.data + rklen, 8);
	}
	else if (p->tag == TAG_LOWER || p->tag == TAG_UPPER)
	{
		if (key->len < BOUND_TRAILER)
			goto bad;
		p->ncols = (int) get_be(key->data + key->len - 3, 2);
		p->columns.data = key->data;
		p->columns.len = key->len - BOUND_TRAILER;
		p->rest.data = NULL;
		p->rest.len = 0;
	}
	else
		goto bad;
	return;

bad:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid prolly index entry")));
}

static void
columns_decode_entry(ProllyIndexKeying *k, const ProllyKey *entry,
					 Datum *values, bool *isnull, ProllyKey *rowkey,
					 uint64 *ordinal)
{
	Parsed		p;

	parse(k, entry, &p);
	if (p.tag != TAG_ENTRY)
		elog(ERROR, "a prolly index bound is not an entry");
	if (values && !PROLLY_CALL(k->keyenc, decode, encoder(k, k->nkeys), &p.columns,
							   values, isnull))
		elog(ERROR, "prolly index entry can't be decoded");
	if (rowkey)
		*rowkey = p.rowkey;
	if (ordinal)
		*ordinal = p.ordinal;
}

static int
compare_bytes(const ProllyKey *a, const ProllyKey *b)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r < 0 ? -1 : 1;
	return (a->len > b->len) - (a->len < b->len);
}

/* The first ncols columns of p's key columns. */
static ProllyKey
columns_prefix(ProllyIndexKeying *k, const Parsed *p, int ncols)
{
	ProllyKey	prefix = p->columns;

	if (ncols < p->ncols)
		prefix.len = PROLLY_CALL(k->keyenc, prefix_length,
								 encoder(k, p->ncols), &p->columns, ncols);
	return prefix;
}

static int
columns_compare(ProllyIndexKeying *k, const ProllyKey *a, const ProllyKey *b)
{
	Parsed		pa;
	Parsed		pb;
	int			n;
	ProllyKey	ca;
	ProllyKey	cb;
	int			r;

	parse(k, a, &pa);
	parse(k, b, &pb);
	n = Min(pa.ncols, pb.ncols);
	ca = columns_prefix(k, &pa, n);
	cb = columns_prefix(k, &pb, n);
	r = PROLLY_CALL(k->keyenc, compare, encoder(k, n), &ca, &cb);
	if (r != 0)
		return r;

	/* The first n columns are equal. */
	if (pa.tag == TAG_ENTRY && pb.tag == TAG_ENTRY)
		return compare_bytes(&pa.rest, &pb.rest);
	if (pa.tag != TAG_ENTRY && pb.tag != TAG_ENTRY)
	{
		if (pa.tag != pb.tag)
			return pa.tag == TAG_LOWER ? -1 : 1;
		return 0;
	}
	if (pa.tag != TAG_ENTRY)
		return pa.tag == TAG_LOWER ? -1 : 1;
	return pb.tag == TAG_LOWER ? 1 : -1;
}

static bool
columns_columns_equal(ProllyIndexKeying *k, const ProllyKey *a,
					  const ProllyKey *b, int ncols)
{
	Parsed		pa;
	Parsed		pb;
	ProllyKey	ca;
	ProllyKey	cb;

	parse(k, a, &pa);
	parse(k, b, &pb);
	ncols = Min(ncols, Min(pa.ncols, pb.ncols));
	ca = columns_prefix(k, &pa, ncols);
	cb = columns_prefix(k, &pb, ncols);
	return PROLLY_CALL(k->keyenc, compare, encoder(k, ncols), &ca, &cb) == 0;
}

static bool
columns_equal_bytes(ProllyIndexKeying *k)
{
	return PROLLY_CALL(k->keyenc, equal_bytes, encoder(k, k->nkeys));
}

static void
columns_close(ProllyIndexKeying *k)
{
	for (int i = 0; i < k->nkeys; i++)
		if (k->prefix[i])
			PROLLY_CALL(k->keyenc, end, k->prefix[i]);
	pfree(k);
}

const ProllyIndexKeyingRoutine prolly_ixkeying_columns_then_row = {
	.info = {
		.iface = PROLLY_IFACE_INDEX_KEYING,
		.impl_id = 1,
		.format_version = 1,
		.name = "columns_then_row",
		.description = "index columns, then the row's key and ordinal",
	},
	.open = columns_open,
	.describe = columns_describe,
	.nkeys = columns_nkeys,
	.encode_entry = columns_encode_entry,
	.decode_entry = columns_decode_entry,
	.encode_bound = columns_encode_bound,
	.compare = columns_compare,
	.columns_equal = columns_columns_equal,
	.equal_bytes = columns_equal_bytes,
	.close = columns_close,
};
