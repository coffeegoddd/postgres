/*-------------------------------------------------------------------------
 *
 * prolly_snapshot.c
 *	  Read-only snapshot mounts: a commit's tables as foreign tables of the
 *	  foreign data wrapper prolly_snapshot, read from the commit's trees.
 *
 * prolly_mount(source_ref, source_schema, target_schema, tables) resolves
 * the ref to one commit and makes a foreign table in target_schema (an
 * ordinary schema: one in a versioned schema would be versioned, and a
 * commit would hold it) for each table of source_schema the commit has (or
 * those named), defined as the commit describes it: its columns, by name,
 * type and collation, and nothing else (a historical constraint isn't
 * enforced, so the optimizer is never told of one).  Each foreign table's
 * options name the mount (target_schema), the commit and the table.  The
 * refs pin the commit under the mount's name, with the mount's source
 * beside it, so a collection keeps every chunk the commit reaches while
 * the mount is there.  prolly_refresh_mount() moves a mount to what a ref
 * names now, and prolly_unmount() drops its tables and its pin; both drop
 * the tables first, so they wait for every scan of them to end.  Mounting
 * reads the commit's root items alone (the canonicalizer's
 * table_metadata()): no row is read, sorted or written, no storage is
 * made and no index is built.
 *
 * A scan reads only what its mount pins (the pin must name the commit its
 * table's options do), checks the reader may read the table's history (as
 * dolt_diff() by name does; the mount checked the table's lineage), and
 * streams the commit's data tree in key order.  On a table keyed by its
 * primary key, equalities on the key's leading columns and a range on the
 * one after them bound the keys read: a seek to the first, and the scan
 * stops past the last.  The quals are all kept as the plan's filter as
 * well, so a bound is only ever an optimization.  The planner is offered
 * the key's order, forward and backward, where each key column's operator
 * family and collation are the column's own; and a scan parameterized by
 * an equality joined to another relation, re-seeking on each rescan.  Only
 * the columns the query uses are decoded (the keying's
 * decode_row_columns()): a long value of a column not used is never read.
 * A table keyed by content gives each copy of a row.
 *
 * A mount made with indexes (prolly_mount(..., indexes => true)) also
 * builds a tree for each btree index the version describes (see "Index
 * trees" below), ordered as the version's index orders its columns, and
 * the pin keeps those trees too.  Their scans seek and order as the
 * primary key's do, reading each row from the data tree unless the
 * index's entries hold every column used.  A refresh brings the trees to
 * the new commit by the rows that differ.  Without indexes, a mount reads
 * no row.
 *
 * A history mount (prolly_mount(..., history => true)) is the table at
 * every commit the mounted one reaches, as dolt_history_table() gives it
 * (see "History" below), its rows streamed a commit at a time: an
 * equality on commit_hash reads that commit alone, and the primary key's
 * bounds seek in each commit.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_snapshot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_datum.h"
#include "access/prolly_keying.h"
#include "access/prolly_refs.h"
#include "access/prolly_snapshot.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "common/hashfn.h"
#include "commands/defrem.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/spi.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#define SNAPSHOT_FDW	"prolly_snapshot"
#define SNAPSHOT_SERVER "prolly_local"

/* A pin's record: its format, then lines of text (mount_info_write()). */
#define PIN_FORMAT		"mount 3"

/* a history mount's columns after the table's: commit_hash, committer, commit_date */
#define HISTORY_COLUMNS 3

/* ------------------------------------------------------------------------
 * Components
 * ------------------------------------------------------------------------
 */

static const ProllyRefsRoutine *
refs(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.refs;
}

static const ProllyCanonRoutine *
canonicalizer(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.canonicalizer;
}

static void
require_snapshots(void)
{
	const ProllyRefsRoutine *r = refs();
	const ProllyCanonRoutine *cn = canonicalizer();

	if (!PROLLY_HAS(r, pin_set) || !PROLLY_HAS(cn, table_metadata) ||
		!PROLLY_HAS(cn, table_names))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't mount its history")));
}

/* ------------------------------------------------------------------------
 * A mount's record, kept with its pin
 * ------------------------------------------------------------------------
 */

/* An index tree a mount built: its table, index, definition and root. */
typedef struct MountIndex
{
	char	   *table;			/* unqualified */
	char	   *index;
	uint64		definition;		/* its statement, hashed */
	ProllyHash	root;
} MountIndex;

typedef struct MountInfo
{
	Oid			owner;			/* the role that mounted it */
	char	   *source_ref;
	char	   *source_schema;
	bool		all;			/* every table of the schema, at each refresh */
	bool		indexes;		/* with its tables' index trees */
	bool		history;		/* its tables at every commit it reaches */
	List	   *tables;			/* unqualified names */
	List	   *built;			/* MountIndex */
} MountInfo;

/*
 * The record: its format, its owner (a local role), the source ref and
 * schema, whether every table, whether indexes and whether history are
 * mounted, then a line per table ("t", a tab, its name) and per index tree
 * ("x", then its root, definition's hash, table and index, tab-separated,
 * the names in hex: a name may hold a tab).  Names never hold a newline:
 * mounts refuse one.
 */
static char *
hex_name(const char *name)
{
	StringInfoData out;

	initStringInfo(&out);
	for (const unsigned char *c = (const unsigned char *) name; *c; c++)
		appendStringInfo(&out, "%02x", *c);
	return out.data;
}

static char *
unhex_name(const char *hex)
{
	size_t		len = strlen(hex);
	char	   *out = palloc(len / 2 + 1);

	if (len % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid snapshot mount record")));
	for (size_t i = 0; i < len / 2; i++)
	{
		unsigned int c;

		if (sscanf(hex + 2 * i, "%2x", &c) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid snapshot mount record")));
		out[i] = (char) c;
	}
	out[len / 2] = '\0';
	return out;
}
static void
mount_info_write(const MountInfo *mi, StringInfo out)
{
	ListCell   *lc;

	appendStringInfo(out, "%s\n%u\n%s\n%s\n%c\n%c\n%c\n", PIN_FORMAT,
					 mi->owner, mi->source_ref, mi->source_schema,
					 mi->all ? 'a' : 'n', mi->indexes ? 'i' : 'n',
					 mi->history ? 'h' : 'n');
	foreach(lc, mi->tables)
		appendStringInfo(out, "t\t%s\n", (char *) lfirst(lc));
	foreach(lc, mi->built)
	{
		MountIndex *x = lfirst(lc);

		appendStringInfo(out, "x\t%s\t%016" PRIx64 "\t%s\t%s\n",
						 prolly_hash_hex(&x->root), x->definition,
						 hex_name(x->table), hex_name(x->index));
	}
}

static void
mount_info_read(const char *data, int len, MountInfo *mi)
{
	char	   *copy = pnstrdup(data, len);
	char	   *line;
	char	   *next;
	int			n = 0;

	memset(mi, 0, sizeof(*mi));
	for (line = copy; line && *line; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		switch (n++)
		{
			case 0:
				if (strcmp(line, PIN_FORMAT) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("a snapshot mount's record is of another format: \"%s\"",
									line),
							 errhint("Unmount it and mount it again.")));
				break;
			case 1:
				mi->owner = (Oid) strtoul(line, NULL, 10);
				break;
			case 2:
				mi->source_ref = pstrdup(line);
				break;
			case 3:
				mi->source_schema = pstrdup(line);
				break;
			case 4:
				mi->all = (line[0] == 'a');
				break;
			case 5:
				mi->indexes = (line[0] == 'i');
				break;
			case 6:
				mi->history = (line[0] == 'h');
				break;
			default:
				if (strncmp(line, "t\t", 2) == 0)
					mi->tables = lappend(mi->tables, pstrdup(line + 2));
				else if (strncmp(line, "x\t", 2) == 0)
				{
					MountIndex *x = palloc0(sizeof(MountIndex));
					char	   *f[4];
					char	   *p = line + 2;

					for (int i = 0; i < 3; i++)
					{
						f[i] = p;
						p = strchr(p, '\t');
						if (p == NULL)
							ereport(ERROR,
									(errcode(ERRCODE_DATA_CORRUPTED),
									 errmsg("invalid snapshot mount record")));
						*p++ = '\0';
					}
					f[3] = p;
					if (!prolly_hash_from_hex(f[0], strlen(f[0]), &x->root))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("invalid snapshot mount record")));
					x->definition = strtou64(f[1], NULL, 16);
					x->table = unhex_name(f[2]);
					x->index = unhex_name(f[3]);
					mi->built = lappend(mi->built, x);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("invalid snapshot mount record")));
				break;
		}
	}
	if (n < 7)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid snapshot mount record")));
}

/*
 * The pin of the mount in schema nsp: named by the schema's OID, which a
 * rename keeps, so a mount follows its schema and no other schema takes
 * its name's pin.
 */
static char *
pin_name(Oid nsp)
{
	return psprintf("%u", nsp);
}

/* The mount in schema nsp: its commit and record; false if none. */
static bool
mount_lookup(Oid nsp, ProllyHash *commit, MountInfo *mi)
{
	StringInfoData info;
	ProllyHash	ignored;
	bool		found;

	if (!OidIsValid(nsp))
		return false;
	initStringInfo(&info);
	found = PROLLY_CALL(refs(), pin_get, pin_name(nsp), commit ? commit : &ignored,
						&info);
	if (found && mi)
		mount_info_read(info.data, info.len, mi);
	pfree(info.data);
	return found;
}

/* Pin commit for the mount in schema nsp, with its record and index trees. */
static void
mount_pin(Oid nsp, const ProllyHash *commit, const MountInfo *mi)
{
	StringInfoData rec;
	ProllyValue recv;
	ProllyHash *keep = palloc(sizeof(ProllyHash) * Max(list_length(mi->built), 1));
	int			nkeep = 0;
	ListCell   *lc;

	initStringInfo(&rec);
	mount_info_write(mi, &rec);
	recv.data = (const uint8 *) rec.data;
	recv.len = rec.len;
	foreach(lc, mi->built)
		keep[nkeep++] = ((MountIndex *) lfirst(lc))->root;
	PROLLY_CALL(refs(), pin_set, pin_name(nsp), commit, keep, nkeep, &recv);
}

static uint64
definition_hash(const char *definition)
{
	return hash_bytes_extended((const unsigned char *) definition,
							   strlen(definition), 0);
}

/* ------------------------------------------------------------------------
 * Index trees
 *
 * A mount's index tree holds an entry for each row of the version (each
 * distinct row of a table keyed by content): its key the index's columns
 * as the key encoder encodes them (under the version's operator families,
 * collations and options) after their length (a varint), then the row's
 * key in the data tree; its value the row with only the columns the index
 * holds (its key and INCLUDE columns, and the primary key's), as the
 * keying makes a tree value of it, with the row's copies.  So entries
 * order by the index's columns, then the data tree's order, and a scan of
 * the index can find the row in the data tree, or read the columns it
 * holds from the entry alone.  A bound to seek by (index_bound()) is the
 * index's columns' bound with a single byte for the row's key: 0 before
 * every row with those columns, 1 after; no row's key is one byte.  An
 * entry is a function of the row and the index's definition alone, so the
 * same version's index has the same tree in any database.
 * ------------------------------------------------------------------------
 */

typedef struct IndexOrder
{
	const ProllyKeyEncoderRoutine *ke;
	ProllyKeyEncoder *ienc;		/* the index's columns */
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;			/* the data tree's keys */
} IndexOrder;

static void
split_entry(const ProllyKey *e, ProllyKey *idx, ProllyKey *row)
{
	const char *p = (const char *) e->data;
	const char *end = p + e->len;
	uint64		len = prolly_get_varint(&p, end);

	if (len > (uint64) (end - p))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid snapshot index entry")));
	idx->data = (const uint8 *) p;
	idx->len = (uint32) len;
	row->data = (const uint8 *) p + len;
	row->len = (uint32) (end - p - len);
}

static int
entry_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	IndexOrder *o = arg;
	ProllyKey	ia;
	ProllyKey	ib;
	ProllyKey	ra;
	ProllyKey	rb;
	int			r;

	split_entry(a, &ia, &ra);
	split_entry(b, &ib, &rb);
	r = PROLLY_CALL(o->ke, compare, o->ienc, &ia, &ib);
	if (r != 0)
		return r;
	/* (a bound's single byte: before or after every row with its columns) */
	if (ra.len == 1 || rb.len == 1)
	{
		int			sa = ra.len == 1 ? (ra.data[0] ? 1 : -1) : 0;
		int			sb = rb.len == 1 ? (rb.data[0] ? 1 : -1) : 0;

		return (sa > sb) - (sa < sb);
	}
	return PROLLY_CALL(o->kr, compare, o->k, &ra, &rb);
}

/* The key encoder of an index's columns, in the table's descriptor. */
static ProllyKeyEncoder *
index_encoder(const ProllyComponents *c, TupleDesc desc,
			  const ProllyHistoricalIndex *ix, MemoryContext mcxt)
{
	ProllyKeyDesc kd;
	Oid		   *types = MemoryContextAlloc(mcxt, sizeof(Oid) * Max(ix->nkeys, 1));

	for (int i = 0; i < ix->nkeys; i++)
		types[i] = TupleDescAttr(desc, ix->attnos[i] - 1)->atttypid;
	kd.nkeys = ix->nkeys;
	kd.types = types;
	kd.opfamilies = ix->opfamilies;
	kd.collations = ix->collations;
	kd.options = ix->options;
	return PROLLY_CALL(c->key_encoder, begin, &kd, mcxt);
}

/* The columns an index's entries hold: its own, INCLUDE's, the key's. */
static bool *
index_covered(const ProllyHistoricalTable *t, const ProllyHistoricalIndex *ix)
{
	bool	   *covered = palloc0(sizeof(bool) * Max(t->desc->natts, 1));

	for (int i = 0; i < ix->nkeys; i++)
		covered[ix->attnos[i] - 1] = true;
	for (int i = 0; i < ix->ninclude; i++)
		covered[ix->include[i] - 1] = true;
	for (int i = 0; t->keyed && i < t->nkeys; i++)
		covered[t->key_attnos[i] - 1] = true;
	return covered;
}

/* The keying of a version's rows: by its primary key, or by content. */
static ProllyKeying *
open_rows(const ProllyComponents *c, const ProllyHistoricalTable *t,
		  const char *source, MemoryContext mcxt)
{
	ProllyKeying *k;

	if (!t->keyed)
		return PROLLY_CALL(c->keying, open_described, t->desc, mcxt);
	if (!PROLLY_HAS(c->keying, open_keyed) ||
		(k = PROLLY_CALL(c->keying, open_keyed, t->desc, t->nkeys,
						 t->key_attnos, t->opfamilies, t->collations,
						 t->options, mcxt)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can't order the keys of \"%s\" as its version stores them",
						source)));
	return k;
}

typedef struct IndexBuild
{
	const ProllyHistoricalTable *t;
	const ProllyHistoricalIndex *ix;
	ProllyComponents c;
	IndexOrder	order;
	ProllyTreeEnv denv;			/* the data tree's */
	ProllyKeyingRefs drefs;
	ProllyTreeEnv ienv;			/* the index tree's */
	ProllyKeyingRefs irefs;
	TupleTableSlot *slot;
	bool	   *keycols;		/* the index's columns, to decode */
	bool	   *covered;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	MemoryContext cxt;
	MemoryContext entries;		/* the entries, and the array of them */
	uint64		used;			/* entries made */
	Size		budget;
} IndexBuild;

static int
data_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	IndexOrder *o = arg;

	return PROLLY_CALL(o->kr, compare, o->k, a, b);
}

static void
index_build_begin(IndexBuild *ib, const ProllyHistoricalTable *t,
				  const ProllyHistoricalIndex *ix, const char *source)
{
	memset(ib, 0, sizeof(*ib));
	ib->t = t;
	ib->ix = ix;
	ib->cxt = CurrentMemoryContext;
	prolly_history_components(&ib->c);
	ib->order.ke = ib->c.key_encoder;
	ib->order.ienc = index_encoder(&ib->c, t->desc, ix, ib->cxt);
	ib->order.kr = ib->c.keying;
	ib->order.k = open_rows(&ib->c, t, source, ib->cxt);
	prolly_folded_tree_env(&ib->denv);
	ib->denv.compare = data_compare;
	ib->denv.compare_arg = &ib->order;
	prolly_keying_use_env(&ib->drefs, ib->c.keying, ib->order.k, ib->c.tree,
						  &ib->denv);
	prolly_folded_tree_env(&ib->ienv);
	ib->ienv.compare = entry_compare;
	ib->ienv.compare_arg = &ib->order;
	prolly_keying_use_env(&ib->irefs, ib->c.keying, ib->order.k, ib->c.tree,
						  &ib->ienv);
	ib->slot = MakeSingleTupleTableSlot(t->desc, &TTSOpsVirtual);
	ib->keycols = palloc0(sizeof(bool) * Max(t->desc->natts, 1));
	for (int i = 0; i < ix->nkeys; i++)
		ib->keycols[ix->attnos[i] - 1] = true;
	ib->covered = index_covered(t, ix);
	ib->budget = (Size) maintenance_work_mem * 1024;
	ib->entries = AllocSetContextCreate(ib->cxt, "prolly snapshot index entries",
										ALLOCSET_DEFAULT_SIZES);
}

/* The entry of a data tree item (its key and tree value). */
static void
index_entry(IndexBuild *ib, const ProllyKey *dkey, const ProllyValue *dvalue,
			ProllyEdit *e)
{
	ProllyValue row;
	uint64		count;
	StringInfoData idx;
	StringInfoData key;
	StringInfoData proj;
	StringInfoData value;
	ProllyValue p;

	PROLLY_CALL(ib->c.keying, parse_value_as, ib->order.k, !ib->t->keyed,
				dvalue, &row, &count);
	PROLLY_CALL(ib->c.keying, decode_row_columns, ib->order.k, &row,
				ib->keycols, ib->slot);
	for (int i = 0; i < ib->ix->nkeys; i++)
	{
		ib->values[i] = ib->slot->tts_values[ib->ix->attnos[i] - 1];
		ib->isnull[i] = ib->slot->tts_isnull[ib->ix->attnos[i] - 1];
	}
	initStringInfo(&idx);
	PROLLY_CALL(ib->c.key_encoder, encode, ib->order.ienc, ib->values,
				ib->isnull, &idx);
	initStringInfo(&key);
	prolly_put_varint(&key, idx.len);
	appendBinaryStringInfo(&key, idx.data, idx.len);
	appendBinaryStringInfo(&key, (const char *) dkey->data, dkey->len);
	initStringInfo(&proj);
	PROLLY_CALL(ib->c.keying, project_row, ib->order.k, &row, ib->covered,
				&proj);
	p.data = (const uint8 *) proj.data;
	p.len = proj.len;
	initStringInfo(&value);
	PROLLY_CALL(ib->c.keying, make_value, ib->order.k, &p, count, &value);
	ExecClearTuple(ib->slot);
	{
		/* (the entry in one allocation of its size) */
		uint8	   *bytes = MemoryContextAlloc(ib->entries, key.len + value.len);

		memcpy(bytes, key.data, key.len);
		memcpy(bytes + key.len, value.data, value.len);
		e->key.data = bytes;
		e->key.len = key.len;
		e->value.data = bytes + key.len;
		e->value.len = value.len;
	}
	e->is_delete = false;
	pfree(idx.data);
	pfree(key.data);
	pfree(proj.data);
	pfree(value.data);
	if ((++ib->used & 63) == 0 &&
		MemoryContextMemAllocated(ib->entries, true) > ib->budget)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index \"%s\" is too large to build within maintenance_work_mem",
						ib->ix->name),
				 errhint("Raise maintenance_work_mem, or mount without indexes.")));
}

static int
edit_compare(const void *a, const void *b, void *arg)
{
	const ProllyEdit *ea = a;
	const ProllyEdit *eb = b;
	int			r = entry_compare(&ea->key, &eb->key, arg);

	/* (a key both removed and added: the addition after) */
	if (r == 0)
		r = (ea->is_delete ? 0 : 1) - (eb->is_delete ? 0 : 1);
	return r;
}

typedef struct DiffEdits
{
	IndexBuild *ib;
	ProllyEdit *edits;
	int			n;
	int			max;
} DiffEdits;

static void
add_edit(DiffEdits *d, const ProllyEdit *e)
{
	if (d->n == d->max)
	{
		d->max = Max(64, d->max * 2);
		d->edits = d->edits ? repalloc_array(d->edits, ProllyEdit, d->max) :
			MemoryContextAlloc(d->ib->entries, sizeof(ProllyEdit) * d->max);
	}
	d->edits[d->n++] = *e;
}

static void
diff_entry(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
		   const ProllyValue *after, void *arg)
{
	DiffEdits  *d = arg;
	ProllyEdit	e;

	CHECK_FOR_INTERRUPTS();
	if (before)
	{
		index_entry(d->ib, key, before, &e);
		e.is_delete = true;
		add_edit(d, &e);
	}
	if (after)
	{
		index_entry(d->ib, key, after, &e);
		add_edit(d, &e);
	}
}

/*
 * The index tree of t's index ix: from old, the index tree of the same
 * index over olddata (a data tree whose rows are keyed and laid out as
 * t's), by the rows that differ, if old isn't NULL; else from every row.
 */
static ProllyHash
build_index(const ProllyHistoricalTable *t, const ProllyHistoricalIndex *ix,
			const char *source, const ProllyNodeRef *olddata,
			const ProllyHash *old)
{
	IndexBuild	ib;
	DiffEdits	d = {0};
	ProllyNodeRef root;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly snapshot index",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext prev = MemoryContextSwitchTo(cxt);
	ProllyHash	result;

	index_build_begin(&ib, t, ix, source);
	d.ib = &ib;
	if (old)
	{
		ProllyNodeRef oldroot = {0};

		oldroot.hash = *old;
		PROLLY_CALL(ib.c.tree, diff, &ib.denv, olddata, &t->data, diff_entry, &d);
		qsort_arg(d.edits, d.n, sizeof(ProllyEdit), edit_compare, &ib.order);

		/* (a key removed and added again: only the addition, its new value) */
		{
			int			w = 0;

			for (int i = 0; i < d.n; i++)
			{
				if (i + 1 < d.n && d.edits[i].is_delete &&
					entry_compare(&d.edits[i].key, &d.edits[i + 1].key,
								  &ib.order) == 0)
					continue;
				d.edits[w++] = d.edits[i];
			}
			d.n = w;
		}
		PROLLY_CALL(ib.c.tree, apply, &ib.ienv, &oldroot, d.edits, d.n, &root);
	}
	else
	{
		ProllyTreeCursor *c = PROLLY_CALL(ib.c.tree, cursor_open, &ib.denv,
										  &t->data, cxt);
		ProllyKey	k;
		ProllyValue v;
		ProllyTreeBuilder *b;

		PROLLY_CALL(ib.c.tree, cursor_seek, c, NULL, ForwardScanDirection);
		while (PROLLY_CALL(ib.c.tree, cursor_next, c, &k, &v))
		{
			ProllyEdit	e;

			CHECK_FOR_INTERRUPTS();
			index_entry(&ib, &k, &v, &e);
			add_edit(&d, &e);
		}
		PROLLY_CALL(ib.c.tree, cursor_close, c);
		qsort_arg(d.edits, d.n, sizeof(ProllyEdit), edit_compare, &ib.order);
		b = PROLLY_CALL(ib.c.tree, build_begin, &ib.ienv, cxt);
		for (int i = 0; i < d.n; i++)
			PROLLY_CALL(ib.c.tree, build_add, b, &d.edits[i].key,
						&d.edits[i].value);
		PROLLY_CALL(ib.c.tree, build_finish, b, &root);
	}
	result = root.hash;
	ExecDropSingleTupleTableSlot(ib.slot);
	PROLLY_CALL(ib.c.key_encoder, end, ib.order.ienc);
	PROLLY_CALL(ib.c.keying, close, ib.order.k);
	MemoryContextSwitchTo(prev);
	MemoryContextDelete(cxt);
	return result;
}

/*
 * Whether index ix's tree built over old's rows (by oix, its definition
 * then) can be brought to new's by the rows that differ: the rows keyed
 * and laid out alike (every column's name, type, typmod and collation),
 * and the index's entries ordered and made alike (its columns, operator
 * families, collations and options, and its INCLUDE columns).  The
 * definition's text alone doesn't tell: it leaves out a collation that is
 * its column's, and names columns, not where the rows hold them.
 */
static bool
same_rows(const ProllyHistoricalTable *old, const ProllyHistoricalTable *new,
		  const ProllyHistoricalIndex *oix, const ProllyHistoricalIndex *ix)
{
	if (oix == NULL || oix->unusable || oix->nkeys != ix->nkeys ||
		oix->ninclude != ix->ninclude || oix->unique != ix->unique)
		return false;
	for (int i = 0; i < ix->nkeys; i++)
		if (oix->attnos[i] != ix->attnos[i] ||
			oix->opfamilies[i] != ix->opfamilies[i] ||
			oix->collations[i] != ix->collations[i] ||
			oix->options[i] != ix->options[i])
			return false;
	for (int i = 0; i < ix->ninclude; i++)
		if (oix->include[i] != ix->include[i])
			return false;
	if (old->keyed != new->keyed || old->nkeys != new->nkeys ||
		old->desc->natts != new->desc->natts)
		return false;
	for (int i = 0; i < old->desc->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(old->desc, i);
		Form_pg_attribute b = TupleDescAttr(new->desc, i);

		if (a->atttypid != b->atttypid || a->atttypmod != b->atttypmod ||
			a->attcollation != b->attcollation ||
			strcmp(NameStr(a->attname), NameStr(b->attname)) != 0)
			return false;
	}
	for (int i = 0; i < old->nkeys; i++)
		if (old->key_attnos[i] != new->key_attnos[i] ||
			old->opfamilies[i] != new->opfamilies[i] ||
			old->collations[i] != new->collations[i] ||
			old->options[i] != new->options[i])
			return false;
	return true;
}

/* ------------------------------------------------------------------------
 * Making a mount's tables
 * ------------------------------------------------------------------------
 */

/*
 * The foreign data wrapper and its server, made the first time a database
 * mounts: by the bootstrap superuser, the server usable by everyone (a
 * table of it a user makes without a mount reads nothing).
 */
static void
ensure_server(void)
{
	Oid			save_userid;
	int			save_sec_context;

	if (OidIsValid(get_foreign_server_oid(SNAPSHOT_SERVER, true)))
		return;
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (!OidIsValid(get_foreign_data_wrapper_oid(SNAPSHOT_FDW, true)) &&
			SPI_execute("CREATE FOREIGN DATA WRAPPER " SNAPSHOT_FDW
						" HANDLER pg_catalog.prolly_snapshot_fdw_handler",
						false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not make the snapshot foreign data wrapper");
		if (SPI_execute("CREATE SERVER " SNAPSHOT_SERVER " FOREIGN DATA WRAPPER "
						SNAPSHOT_FDW, false, 0) != SPI_OK_UTILITY ||
			SPI_execute("GRANT USAGE ON FOREIGN SERVER " SNAPSHOT_SERVER
						" TO PUBLIC", false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not make the snapshot server");
		SPI_finish();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
}

/* The commit ref names, refused if it's the working set or names none. */
static void
resolve_commit(const char *ref, ProllyHash *commit, ProllyCommitInfo *info)
{
	if (pg_strcasecmp(ref, "WORKING") == 0 || pg_strcasecmp(ref, "STAGED") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a snapshot mount reads a commit, not the working set"),
				 errhint("Commit the working set, then mount the commit.")));
	if (!PROLLY_CALL(refs(), resolve, ref, commit))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	if (!PROLLY_CALL(refs(), commit_get, commit, info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", prolly_hash_hex(commit))));
}

static void
check_name(const char *name, const char *what)
{
	if (strchr(name, '\n') != NULL || name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid %s name \"%s\"", what, name)));
}

static void
run_ddl(const char *sql)
{
	if (SPI_execute(sql, false, 0) != SPI_OK_UTILITY)
		elog(ERROR, "could not run \"%s\"", sql);
}

/* The index tree old (a mount's record before) has of table's index. */
static MountIndex *
built_before(const MountInfo *old, const char *table, const char *index,
			 uint64 definition)
{
	ListCell   *lc;

	foreach(lc, old ? old->built : NIL)
	{
		MountIndex *x = lfirst(lc);

		if (strcmp(x->table, table) == 0 && strcmp(x->index, index) == 0 &&
			x->definition == definition)
			return x;
	}
	return NULL;
}

/*
 * The index trees of table (t, at commit) for mi: each index of the
 * version that can be followed, brought from old's (the mount's record
 * before a refresh, at oldroot, or NULL) by the rows that differ where
 * old has it and the rows are alike, else built from every row.
 */
static void
make_indexes(MountInfo *mi, const char *table, const char *name,
			 const ProllyHistoricalTable *t, const MountInfo *old,
			 const ProllyNodeRef *oldroot, MemoryContext cxt)
{
	ListCell   *lc;

	foreach(lc, t->indexes)
	{
		ProllyHistoricalIndex *ix = lfirst(lc);
		uint64		def;
		MountIndex *before;
		MountIndex *x;
		ProllyHash	root;
		ProllyHistoricalTable ot;
		bool		incremental = false;
		MemoryContext prev;

		if (ix->unusable || strchr(ix->name, '\n'))
			continue;
		def = definition_hash(ix->definition);
		before = built_before(old, table, ix->name, def);
		if (before && oldroot)
		{
			bool		found;

			if (PROLLY_CALL(canonicalizer(), table_metadata, oldroot, name,
							&ot, &found) == NULL && found)
			{
				ListCell   *ol;

				foreach(ol, ot.indexes)
					if (strcmp(((ProllyHistoricalIndex *) lfirst(ol))->name,
							   ix->name) == 0)
						incremental = same_rows(&ot, t, lfirst(ol), ix);
			}
		}
		root = build_index(t, ix, name, incremental ? &ot.data : NULL,
						   incremental ? &before->root : NULL);
		prev = MemoryContextSwitchTo(cxt);
		x = palloc0(sizeof(MountIndex));
		x->table = pstrdup(table);
		x->index = pstrdup(ix->name);
		x->definition = def;
		x->root = root;
		mi->built = lappend(mi->built, x);
		MemoryContextSwitchTo(prev);
	}
}

/*
 * Make mi's tables of commit (root) in target, as the user: each foreign
 * table defined as the commit describes its table, and, if mi has
 * indexes, its index trees (old: the mount's record before a refresh, at
 * oldroot).  Refused, before any is made, if one can't be read here as
 * stored, or the user may not read it.
 */
static void
make_tables(const char *target, const ProllyHash *commit,
			const ProllyNodeRef *root, MountInfo *mi, const MountInfo *old,
			const ProllyNodeRef *oldroot, MemoryContext cxt, const char *fn)
{
	char	   *hex = prolly_hash_hex(commit);
	List	   *defs = NIL;
	ListCell   *lc;

	if (mi->all)
	{
		List	   *names = PROLLY_CALL(canonicalizer(), table_names, root,
										mi->source_schema);

		MemoryContext prev = MemoryContextSwitchTo(cxt);

		/* (the record outlives SPI's memory) */
		mi->tables = NIL;
		foreach(lc, names)
		{
			List	   *parts = stringToQualifiedNameList(lfirst(lc), NULL);

			mi->tables = lappend(mi->tables, pstrdup(strVal(lsecond(parts))));
		}
		MemoryContextSwitchTo(prev);
		if (mi->tables == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("%s has no tables in schema \"%s\"", mi->source_ref,
							mi->source_schema)));
	}
	mi->built = NIL;
	foreach(lc, mi->tables)
	{
		const char *table = lfirst(lc);
		char	   *name = quote_qualified_identifier(mi->source_schema, table);
		ProllyHistoricalTable t;
		bool		found;
		const char *why;
		StringInfoData sql;

		check_name(table, "table");
		why = PROLLY_CALL(canonicalizer(), table_metadata, root, name, &t, &found);
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("%s has no table \"%s\"", mi->source_ref, name)));
		if (why)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s can't mount \"%s\": %s", fn, name, why)));
		prolly_check_history_by_name(name, hex, true, fn);
		if (mi->indexes)
			make_indexes(mi, table, name, &t, old, oldroot, cxt);
		for (int i = 0; mi->history && i < t.desc->natts; i++)
		{
			const char *col = NameStr(TupleDescAttr(t.desc, i)->attname);

			if (strcmp(col, "commit_hash") == 0 || strcmp(col, "committer") == 0 ||
				strcmp(col, "commit_date") == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("%s can't mount the history of \"%s\": it has a column \"%s\"",
								fn, name, col),
						 errdetail("A history mount's table adds commit_hash, committer and commit_date.")));
		}

		initStringInfo(&sql);
		appendStringInfo(&sql, "CREATE FOREIGN TABLE %s (",
						 quote_qualified_identifier(target, table));
		for (int i = 0; i < t.desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(t.desc, i);

			appendStringInfo(&sql, "%s%s %s", i > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)),
							 format_type_with_typemod(att->atttypid, att->atttypmod));
			if (OidIsValid(att->attcollation) &&
				att->attcollation != get_typcollation(att->atttypid))
				appendStringInfo(&sql, " COLLATE %s",
								 generate_collation_name(att->attcollation));
		}
		if (mi->history)
			appendStringInfoString(&sql, ", commit_hash text, committer text, "
								   "commit_date timestamptz");
		appendStringInfo(&sql, ") SERVER " SNAPSHOT_SERVER
						 " OPTIONS (commit %s, source %s%s)",
						 quote_literal_cstr(hex), quote_literal_cstr(name),
						 mi->history ? ", history 'true'" : "");
		defs = lappend(defs, sql.data);
	}
	foreach(lc, defs)
		run_ddl(lfirst(lc));
}

/* Drop mi's tables in target, those still there. */
static void
drop_tables(const char *target, const MountInfo *mi)
{
	ListCell   *lc;

	foreach(lc, mi->tables)
		run_ddl(psprintf("DROP FOREIGN TABLE IF EXISTS %s",
						 quote_qualified_identifier(target, lfirst(lc))));
}

/* Refuse a change to a mount by anyone but the role that made it. */
static void
check_mount_owner(const char *target, const MountInfo *mi)
{
	if (mi->owner != GetUserId() && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be the owner of snapshot mount \"%s\"", target)));
}

/*
 * Refuse a mount's target the user may not make it in: a system schema
 * (one named pg_*, or information_schema), or one the user doesn't own.
 */
static void
check_target(const char *target)
{
	Oid			nsp = get_namespace_oid(target, true);

	if (strncmp(target, "pg_", 3) == 0 || strcmp(target, "information_schema") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("a snapshot mount can't be in system schema \"%s\"", target)));
	if (OidIsValid(nsp) &&
		!object_ownercheck(NamespaceRelationId, nsp, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA, target);
	if (!OidIsValid(nsp) &&
		object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
						ACL_CREATE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));
}

/*
 * prolly_mount(source_ref text, source_schema text, target_schema text,
 *				tables text[] DEFAULT NULL, indexes boolean DEFAULT false,
 *				history boolean DEFAULT false, OUT mount text, OUT commit text)
 *	  returns record
 */
Datum
prolly_mount(PG_FUNCTION_ARGS)
{
	char	   *ref;
	char	   *target;
	MountInfo	mi = {0};
	ProllyHash	commit;
	ProllyCommitInfo info;
	Oid			nsp;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	MemoryContext cxt;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("prolly_mount's ref and schemas can't be null")));
	require_snapshots();
	ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	mi.source_schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	target = text_to_cstring(PG_GETARG_TEXT_PP(2));
	check_name(ref, "ref");
	check_name(mi.source_schema, "schema");
	check_name(target, "schema");
	mi.source_ref = ref;
	mi.owner = GetUserId();
	mi.all = PG_ARGISNULL(3);
	mi.indexes = !PG_ARGISNULL(4) && PG_GETARG_BOOL(4);
	mi.history = !PG_ARGISNULL(5) && PG_GETARG_BOOL(5);
	if (mi.indexes && mi.history)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a history mount can't build index trees"),
				 errhint("Mount the history without indexes, or a commit with them.")));
	if (!mi.all)
	{
		Datum	   *elems;
		bool	   *enulls;
		int			n;

		deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(3), TEXTOID, &elems,
								  &enulls, &n);
		for (int i = 0; i < n; i++)
		{
			if (enulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("prolly_mount's tables can't be null")));
			mi.tables = lappend(mi.tables, TextDatumGetCString(elems[i]));
		}
	}
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (!mi.all && mi.tables == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("prolly_mount's tables can't be empty")));
	check_target(target);
	resolve_commit(ref, &commit, &info);
	if (mount_lookup(get_namespace_oid(target, true), NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("schema \"%s\" is a snapshot mount already", target),
				 errhint("Refresh it with prolly_refresh_mount(), or unmount it first.")));
	ensure_server();
	cxt = CurrentMemoryContext;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	nsp = get_namespace_oid(target, true);
	if (!OidIsValid(nsp))
	{
		run_ddl(psprintf("CREATE SCHEMA %s", quote_identifier(target)));
		CommandCounterIncrement();
		nsp = get_namespace_oid(target, false);
	}
	if (namespace_is_versioned(nsp))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("a snapshot mount can't be in versioned schema \"%s\"", target),
				 errdetail("A commit would hold its tables' definitions.")));
	make_tables(target, &commit, &info.root, &mi, NULL, NULL, cxt,
				"prolly_mount");
	SPI_finish();
	mount_pin(nsp, &commit, &mi);

	values[0] = CStringGetTextDatum(target);
	values[1] = CStringGetTextDatum(prolly_hash_hex(&commit));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc),
													  values, nulls)));
}

/*
 * prolly_refresh_mount(mount text, source_ref text DEFAULT NULL)
 *	  returns text
 *
 * The mount moved to the commit source_ref names now (NULL: the ref it was
 * mounted from): its tables dropped and made again, as of that commit,
 * and its index trees brought there by the rows that differ.
 */
Datum
prolly_refresh_mount(PG_FUNCTION_ARGS)
{
	char	   *target;
	Oid			nsp;
	MountInfo	mi;
	MountInfo	old;
	ProllyHash	oldcommit;
	ProllyCommitInfo oldinfo;
	ProllyHash	commit;
	ProllyCommitInfo info;
	MemoryContext cxt;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("prolly_refresh_mount's mount can't be null")));
	require_snapshots();
	target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	nsp = get_namespace_oid(target, false);
	if (!mount_lookup(nsp, &oldcommit, &mi))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("schema \"%s\" is not a snapshot mount", target)));
	old = mi;
	check_mount_owner(target, &mi);
	if (!PG_ARGISNULL(1))
	{
		mi.source_ref = text_to_cstring(PG_GETARG_TEXT_PP(1));
		check_name(mi.source_ref, "ref");
	}
	resolve_commit(mi.source_ref, &commit, &info);
	if (!PROLLY_CALL(refs(), commit_get, &oldcommit, &oldinfo))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", prolly_hash_hex(&oldcommit))));
	cxt = CurrentMemoryContext;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	drop_tables(target, &old);
	CommandCounterIncrement();
	make_tables(target, &commit, &info.root, &mi, &old, &oldinfo.root, cxt,
				"prolly_refresh_mount");
	SPI_finish();
	mount_pin(nsp, &commit, &mi);
	PG_RETURN_TEXT_P(cstring_to_text(prolly_hash_hex(&commit)));
}

/*
 * prolly_unmount(mount text) returns void
 *
 * The mount's tables dropped, then its pin: the commit and the mount's
 * index trees are collected once nothing else keeps them.
 */
Datum
prolly_unmount(PG_FUNCTION_ARGS)
{
	char	   *target;
	Oid			nsp;
	MountInfo	mi;
	ProllyHash	commit;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("prolly_unmount's mount can't be null")));
	require_snapshots();
	target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	nsp = get_namespace_oid(target, false);
	if (!mount_lookup(nsp, &commit, &mi))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("schema \"%s\" is not a snapshot mount", target)));
	check_mount_owner(target, &mi);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	drop_tables(target, &mi);
	SPI_finish();
	PROLLY_CALL(refs(), pin_set, pin_name(nsp), NULL, NULL, 0, NULL);
	PG_RETURN_VOID();
}

/*
 * A schema nsp dropped (doDeletion()): if it is a snapshot mount's, the
 * mount goes with it, its pin released, as unmounting it would.
 */
void
prolly_snapshot_schema_dropped(Oid nsp)
{
	ProllyHash	commit;
	const ProllyRefsRoutine *r;

	if (!IsNormalProcessingMode() || namespace_is_versioned(nsp))
		return;
	r = refs();
	if (!PROLLY_HAS(r, pin_get))
		return;
	if (PROLLY_CALL(r, pin_get, pin_name(nsp), &commit, NULL))
		PROLLY_CALL(r, pin_set, pin_name(nsp), NULL, NULL, 0, NULL);
}

typedef struct MountsVisit
{
	ReturnSetInfo *rsinfo;
} MountsVisit;

static void
visit_mount(const char *name, const ProllyHash *commit, const ProllyValue *info,
			void *arg)
{
	MountsVisit *mv = arg;
	MountInfo	mi;
	Datum		values[7];
	bool		nulls[7] = {0};
	Datum	   *elems;
	int			n = 0;
	ListCell   *lc;

	mount_info_read((const char *) info->data, info->len, &mi);
	elems = palloc(sizeof(Datum) * Max(list_length(mi.tables), 1));
	foreach(lc, mi.tables)
		elems[n++] = CStringGetTextDatum(lfirst(lc));
	{
		char	   *schema = get_namespace_name((Oid) strtoul(name, NULL, 10));

		values[0] = CStringGetTextDatum(schema ? schema : name);
	}
	values[1] = CStringGetTextDatum(prolly_hash_hex(commit));
	values[2] = CStringGetTextDatum(mi.source_ref);
	values[3] = CStringGetTextDatum(mi.source_schema);
	values[4] = PointerGetDatum(construct_array_builtin(elems, n, TEXTOID));
	elems = palloc(sizeof(Datum) * Max(list_length(mi.built), 1));
	n = 0;
	foreach(lc, mi.built)
	{
		MountIndex *x = lfirst(lc);

		elems[n++] = CStringGetTextDatum(psprintf("%s.%s",
												  quote_identifier(x->table),
												  quote_identifier(x->index)));
	}
	values[5] = PointerGetDatum(construct_array_builtin(elems, n, TEXTOID));
	values[6] = BoolGetDatum(mi.history);
	tuplestore_putvalues(mv->rsinfo->setResult, mv->rsinfo->setDesc, values,
						 nulls);
}

/*
 * prolly_mounts(OUT mount text, OUT commit text, OUT source_ref text,
 *				 OUT source_schema text, OUT tables text[], OUT indexes text[],
 *				 OUT history boolean)
 *	  returns setof record
 */
Datum
prolly_mounts(PG_FUNCTION_ARGS)
{
	MountsVisit mv;

	require_snapshots();
	InitMaterializedSRF(fcinfo, 0);
	mv.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PROLLY_CALL(refs(), pins, visit_mount, &mv);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * A mounted table, as a scan opens it
 * ------------------------------------------------------------------------
 */

/*
 * A key a scan can seek by: the primary key's (the data tree), or a built
 * index's (its tree), its columns as the version orders them.
 */
typedef struct MountKey
{
	const ProllyHistoricalIndex *ix;	/* NULL: the primary key */
	int			nkeys;
	const AttrNumber *attnos;
	const Oid  *opfamilies;
	const Oid  *collations;
	const int16 *options;
	bool		unique;
	ProllyNodeRef root;			/* its tree */
	bool	   *covered;		/* (an index) the columns its entries hold */
} MountKey;

typedef struct Mounted
{
	char	   *mount;
	char	   *source;			/* the table's name in the commit */
	ProllyHash	commit;
	ProllyHistoricalTable t;
	double		count;			/* the tree's items */
	int			nkeys;
	MountKey   *keys;			/* the primary key's first, if keyed */
	bool		history;		/* every commit's rows, with the commit's */
} Mounted;

/*
 * The mounted table relid's snapshot: its options, checked against the
 * pin of the mount in its schema, and the table as the commit describes it, checked against
 * the foreign table's columns; and the keys it can seek by.
 */
static void
snapshot_open(Oid relid, Mounted *s)
{
	ForeignTable *ft = GetForeignTable(relid);
	char	   *hex = NULL;
	ProllyHash	pinned;
	ProllyCommitInfo info;
	MountInfo	mi;
	ListCell   *lc;
	bool		found;
	const char *why;
	Relation	rel;
	TupleDesc	desc;
	char	   *table = NULL;

	require_snapshots();
	memset(s, 0, sizeof(*s));
	foreach(lc, ft->options)
	{
		DefElem    *d = lfirst(lc);

		if (strcmp(d->defname, "commit") == 0)
			hex = defGetString(d);
		else if (strcmp(d->defname, "source") == 0)
			s->source = defGetString(d);
		else if (strcmp(d->defname, "history") == 0)
			s->history = defGetBoolean(d);
	}
	s->mount = get_namespace_name(get_rel_namespace(relid));
	if (hex == NULL || s->source == NULL ||
		!prolly_hash_from_hex(hex, strlen(hex), &s->commit))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("foreign table \"%s\" isn't a snapshot mount's table",
						get_rel_name(relid)),
				 errhint("Make snapshot tables with prolly_mount().")));
	if (!mount_lookup(get_rel_namespace(relid), &pinned, &mi) ||
		pinned.len != s->commit.len ||
		memcmp(pinned.bytes, s->commit.bytes, pinned.len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("snapshot mount \"%s\" doesn't pin the commit of table \"%s\"",
						s->mount, get_rel_name(relid)),
				 errhint("Mount the commit with prolly_mount(), or refresh the mount.")));
	/* (the mount's own tables alone: those it checked could be read) */
	foreach(lc, mi.tables)
		if (strcmp(quote_qualified_identifier(mi.source_schema, lfirst(lc)),
				   s->source) == 0)
			table = lfirst(lc);
	if (table == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("snapshot mount \"%s\" doesn't mount \"%s\", which table \"%s\" names",
						s->mount, s->source, get_rel_name(relid))));
	if (!PROLLY_CALL(refs(), commit_get, &s->commit, &info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", hex)));
	why = PROLLY_CALL(canonicalizer(), table_metadata, &info.root, s->source,
					  &s->t, &found);
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("commit %s has no table \"%s\"", hex, s->source)));
	if (why)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can't read \"%s\" as commit %s stores it: %s",
						s->source, hex, why)));

	/* the foreign table's columns must still be the version's */
	rel = table_open(relid, NoLock);
	desc = RelationGetDescr(rel);
	found = desc->natts == s->t.desc->natts + (s->history ? HISTORY_COLUMNS : 0);
	for (int i = s->t.desc->natts; found && i < desc->natts; i++)
		found = !TupleDescAttr(desc, i)->attisdropped &&
			TupleDescAttr(desc, i)->atttypid ==
			(i == desc->natts - 1 ? TIMESTAMPTZOID : TEXTOID);
	for (int i = 0; found && i < s->t.desc->natts; i++)
		found = !TupleDescAttr(desc, i)->attisdropped &&
			TupleDescAttr(desc, i)->atttypid == TupleDescAttr(s->t.desc, i)->atttypid &&
			TupleDescAttr(desc, i)->atttypmod == TupleDescAttr(s->t.desc, i)->atttypmod;
	table_close(rel, NoLock);
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("foreign table \"%s\" no longer has the columns of \"%s\" as its commit has them",
						get_rel_name(relid), s->source),
				 errhint("Refresh its mount with prolly_refresh_mount().")));

	/* the keys: the primary key, then each index the mount built */
	s->keys = palloc0(sizeof(MountKey) * (1 + list_length(s->t.indexes)));
	if (s->t.keyed)
	{
		MountKey   *k = &s->keys[s->nkeys++];

		k->nkeys = s->t.nkeys;
		k->attnos = s->t.key_attnos;
		k->opfamilies = s->t.opfamilies;
		k->collations = s->t.collations;
		k->options = s->t.options;
		k->unique = true;
		k->root = s->t.data;
	}
	foreach(lc, s->t.indexes)
	{
		ProllyHistoricalIndex *ix = lfirst(lc);
		MountIndex *x;
		MountKey   *k;

		if (ix->unusable || s->history)
			continue;
		x = built_before(&mi, table, ix->name, definition_hash(ix->definition));
		if (x == NULL)
			continue;
		k = &s->keys[s->nkeys++];
		k->ix = ix;
		k->nkeys = ix->nkeys;
		k->attnos = ix->attnos;
		k->opfamilies = ix->opfamilies;
		k->collations = ix->collations;
		k->options = ix->options;
		k->unique = ix->unique;
		k->root.hash = x->root;
		k->covered = index_covered(&s->t, ix);
	}
}

/* ------------------------------------------------------------------------
 * History
 *
 * A history mount's table has the version's columns, then commit_hash,
 * committer and commit_date: a row for each row of the table at each
 * commit the mounted commit reaches (that has the table), as
 * dolt_history_table(NULL::t) gives them: the commit's table of the name
 * (one renamed since isn't followed), each read as its own commit defines
 * it, its columns by identity (renamed since by their names there,
 * retyped since converted: a value that doesn't convert is NULL, with a
 * warning once a scan; one the commit hasn't is NULL).  A commit whose
 * table can't be read here is an error, as is one missing from the store;
 * a reader is refused a commit whose table isn't the one here, as
 * dolt_history_table() by name refuses it.  A scan visits the commits one
 * at a time and streams each one's rows, so LIMIT stops it; an equality
 * on commit_hash visits that commit alone, if the mounted commit reaches
 * it (found by walking down to its height, kept for rescans); and the
 * primary key's bounds seek in each commit whose key is the version's.
 * ------------------------------------------------------------------------
 */


/*
 * Whether clause is commit_hash = expr (or expr = commit_hash), expr not
 * of rel and not volatile: the commit it selects.
 */
static bool
commit_clause(RelOptInfo *rel, const Mounted *s, RestrictInfo *ri, Expr **expr)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	AttrNumber	attno = s->t.desc->natts + 1;

	if (!IsA(ri->clause, OpExpr))
		return false;
	op = (OpExpr *) ri->clause;
	if (op->opno != TextEqualOperator || list_length(op->args) != 2)
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	if (!(IsA(left, Var) && ((Var *) left)->varno == rel->relid &&
		  ((Var *) left)->varattno == attno && ((Var *) left)->varlevelsup == 0))
	{
		Node	   *tmp = left;

		left = right;
		right = tmp;
		if (!(IsA(left, Var) && ((Var *) left)->varno == rel->relid &&
			  ((Var *) left)->varattno == attno &&
			  ((Var *) left)->varlevelsup == 0))
			return false;
	}
	if (bms_is_member(rel->relid, pull_varnos(NULL, right)) ||
		contain_volatile_functions(right))
		return false;
	*expr = (Expr *) right;
	return true;
}


/* ------------------------------------------------------------------------
 * Key bounds
 * ------------------------------------------------------------------------
 */

/*
 * What a path's quals bound of a key: the expressions the leading neq key
 * columns equal, then a lower and an upper bound (in SQL's order, not the
 * tree's) on the column after them.
 */
typedef struct KeyBounds
{
	int			neq;
	Expr	   *eq[INDEX_MAX_KEYS];
	Expr	   *lo;
	bool		lo_strict;
	Expr	   *hi;
	bool		hi_strict;
} KeyBounds;

/*
 * The integer types, whose cross-type btree operators (int84eq and the
 * like) bound a key: a value of one converts exactly to another, or is out
 * of its range (bound_value()).
 */
static bool
integer_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID;
}

/*
 * A bound's value d of type from as the key column's type to: false if it
 * is out of that type's range (no key equals it).
 */
static bool
bound_value(Datum d, Oid from, Oid to, Datum *out)
{
	int64		v;

	if (from == to)
	{
		*out = d;
		return true;
	}
	v = from == INT2OID ? DatumGetInt16(d) : from == INT4OID ?
		DatumGetInt32(d) : DatumGetInt64(d);
	if (to == INT2OID && v >= PG_INT16_MIN && v <= PG_INT16_MAX)
		*out = Int16GetDatum((int16) v);
	else if (to == INT4OID && v >= PG_INT32_MIN && v <= PG_INT32_MAX)
		*out = Int32GetDatum((int32) v);
	else if (to == INT8OID)
		*out = Int64GetDatum(v);
	else
		return false;
	return true;
}

/*
 * Whether clause bounds column c of key k of rel: Var op expr (or expr op
 * Var), op a btree operator of the column's family for its type on the
 * left and on the right (or, for integers, another integer type), under
 * the key column's collation, expr not of rel and not volatile.  Sets the
 * strategy (as Var op expr) and expr.
 */
static bool
key_clause(RelOptInfo *rel, const Mounted *s, const MountKey *k, int c,
		   RestrictInfo *ri, int *strategy, Expr **expr)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Oid			opno;
	AttrNumber	attno = k->attnos[c];
	Oid			type = TupleDescAttr(s->t.desc, attno - 1)->atttypid;
	int			strat;
	Oid			lefttype;
	Oid			righttype;

	if (!IsA(ri->clause, OpExpr))
		return false;
	op = (OpExpr *) ri->clause;
	if (list_length(op->args) != 2)
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	opno = op->opno;
	if (!(IsA(left, Var) && ((Var *) left)->varno == rel->relid &&
		  ((Var *) left)->varattno == attno && ((Var *) left)->varlevelsup == 0))
	{
		Node	   *tmp = left;

		left = right;
		right = tmp;
		opno = get_commutator(opno);
		if (!OidIsValid(opno) ||
			!(IsA(left, Var) && ((Var *) left)->varno == rel->relid &&
			  ((Var *) left)->varattno == attno &&
			  ((Var *) left)->varlevelsup == 0))
			return false;
	}
	if (bms_is_member(rel->relid, pull_varnos(NULL, right)) ||
		contain_volatile_functions(right))
		return false;
	if (!op_in_opfamily(opno, k->opfamilies[c]))
		return false;
	get_op_opfamily_properties(opno, k->opfamilies[c], false, &strat,
							   &lefttype, &righttype);
	if (lefttype != type || exprType(right) != righttype ||
		(righttype != type && !(integer_type(type) && integer_type(righttype))))
		return false;
	if (OidIsValid(k->collations[c]) && op->inputcollid != k->collations[c])
		return false;
	*strategy = strat;
	*expr = (Expr *) right;
	return true;
}

/* The bounds clauses (RestrictInfos) put on key k of rel. */
static void
key_bounds(RelOptInfo *rel, const Mounted *s, const MountKey *k, List *clauses,
		   KeyBounds *b)
{
	memset(b, 0, sizeof(*b));
	for (int c = 0; c < k->nkeys; c++)
	{
		ListCell   *lc;
		bool		eq = false;

		foreach(lc, clauses)
		{
			int			strat;
			Expr	   *e;

			if (!key_clause(rel, s, k, c, lfirst(lc), &strat, &e))
				continue;
			if (strat == BTEqualStrategyNumber)
			{
				b->eq[c] = e;
				eq = true;
				break;
			}
		}
		if (eq)
		{
			b->neq++;
			continue;
		}
		foreach(lc, clauses)
		{
			int			strat;
			Expr	   *e;

			if (!key_clause(rel, s, k, c, lfirst(lc), &strat, &e))
				continue;
			if ((strat == BTGreaterStrategyNumber ||
				 strat == BTGreaterEqualStrategyNumber) && b->lo == NULL)
			{
				b->lo = e;
				b->lo_strict = (strat == BTGreaterStrategyNumber);
			}
			else if ((strat == BTLessStrategyNumber ||
					  strat == BTLessEqualStrategyNumber) && b->hi == NULL)
			{
				b->hi = e;
				b->hi_strict = (strat == BTLessStrategyNumber);
			}
		}
		break;
	}
}

/* ------------------------------------------------------------------------
 * Seeking a key's tree
 * ------------------------------------------------------------------------
 */

/*
 * A key's tree, opened for seeking: the data tree, ordered by the rows'
 * keying, or an index's, ordered by its entries (entry_compare()).
 */
typedef struct KeyTree
{
	const Mounted *s;
	const MountKey *key;
	ProllyComponents c;
	ProllyKeying *k;			/* the data tree's keys */
	IndexOrder	order;			/* an index's */
	ProllyTreeEnv env;			/* the data tree's */
	ProllyKeyingRefs refs;
	ProllyTreeEnv ienv;			/* an index's */
	ProllyKeyingRefs irefs;
} KeyTree;

static int
data_key_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	KeyTree    *kt = arg;

	return PROLLY_CALL(kt->c.keying, compare, kt->k, a, b);
}

static void
key_tree_open(KeyTree *kt, const Mounted *s, const MountKey *key,
			  MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	memset(kt, 0, sizeof(*kt));
	kt->s = s;
	kt->key = key;
	prolly_history_components(&kt->c);
	kt->k = open_rows(&kt->c, &s->t, s->source, mcxt);
	prolly_folded_tree_env(&kt->env);
	kt->env.compare = data_key_compare;
	kt->env.compare_arg = kt;
	prolly_keying_use_env(&kt->refs, kt->c.keying, kt->k, kt->c.tree, &kt->env);
	if (key && key->ix)
	{
		kt->order.ke = kt->c.key_encoder;
		kt->order.ienc = index_encoder(&kt->c, s->t.desc, key->ix, mcxt);
		kt->order.kr = kt->c.keying;
		kt->order.k = kt->k;
		prolly_folded_tree_env(&kt->ienv);
		kt->ienv.compare = entry_compare;
		kt->ienv.compare_arg = &kt->order;
		prolly_keying_use_env(&kt->irefs, kt->c.keying, kt->k, kt->c.tree,
							  &kt->ienv);
	}
	MemoryContextSwitchTo(old);
}

static void
key_tree_close(KeyTree *kt)
{
	if (kt->key && kt->key->ix)
		PROLLY_CALL(kt->c.key_encoder, end, kt->order.ienc);
	PROLLY_CALL(kt->c.keying, close, kt->k);
}

/* The env of the key's tree. */
static const ProllyTreeEnv *
key_tree_env(const KeyTree *kt)
{
	return kt->key && kt->key->ix ? &kt->ienv : &kt->env;
}

static int
key_tree_compare(KeyTree *kt, const ProllyKey *a, const ProllyKey *b)
{
	if (kt->key && kt->key->ix)
		return entry_compare(a, b, &kt->order);
	return PROLLY_CALL(kt->c.keying, compare, kt->k, a, b);
}

/*
 * A seek's bounds, as keys of the tree's order: what a scan computes from
 * its bound expressions' values, and the planner from constants, to count
 * the keys between them.
 */
typedef struct Seek
{
	KeyTree    *kt;
	int			neq;
	bool		has_lo;
	bool		has_hi;
	bool		lo_strict;		/* (in SQL's order) */
	bool		hi_strict;

	StringInfo	tree_lo;		/* NULL: none */
	bool		tree_lo_strict;
	StringInfo	tree_hi;
	bool		tree_hi_strict;
	bool		empty;			/* no key can be in them */
} Seek;

/*
 * One of the tree's bounds: the equality prefix, then v (if has_v), as the
 * least key with them (high: the greatest).  On an index, the bound of its
 * columns with the byte that puts it before (after) every row's.
 */
static StringInfo
tree_bound(Seek *sk, Datum *values, bool *isnull, bool has_v, Datum v,
		   bool high)
{
	KeyTree    *kt = sk->kt;
	StringInfo	key = makeStringInfo();
	int			n = sk->neq;

	if (has_v)
	{
		values[n] = v;
		isnull[n] = false;
		n++;
	}
	if (kt->key->ix)
	{
		StringInfoData idx;

		initStringInfo(&idx);
		PROLLY_CALL(kt->c.key_encoder, encode_bound, kt->order.ienc, values,
					isnull, n, high, &idx);
		prolly_put_varint(key, idx.len);
		appendBinaryStringInfo(key, idx.data, idx.len);
		appendStringInfoChar(key, high ? 1 : 0);
	}
	else
		PROLLY_CALL(kt->c.keying, encode_key_bound, kt->k, values, isnull, n,
					high, key);
	return key;
}

/*
 * The sign of an integer bound's value: which way it is out of a smaller
 * type's range.
 */
static int
integer_sign(Datum d, Oid type)
{
	int64		v = type == INT2OID ? DatumGetInt16(d) : type == INT4OID ?
		DatumGetInt32(d) : DatumGetInt64(d);

	return v > 0 ? 1 : -1;
}

/*
 * The bounds as keys of the tree's order, from the bounds' values (each
 * NULL where nulls says, of the types types say: the equalities', then the
 * lower bound's and the upper's, those the seek has), in the current
 * memory context.  A strict lower bound is the greatest key with its
 * values (keys after it are past them), an inclusive one the least; an
 * upper bound the other way round.  On a DESC column SQL's lower bound is
 * the tree's upper.  No key equals NULL or is past it, nor equals an
 * integer out of its column's range; a range bound out of it is either
 * past every key or none.
 */
static void
compute_bounds(Seek *sk, const Datum *in, const bool *nulls, const Oid *types)
{
	const MountKey *key = sk->kt->key;
	const Mounted *s = sk->kt->s;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			i = 0;
	Datum		lo = 0;
	Datum		hi = 0;
	bool		has_lo = sk->has_lo;
	bool		has_hi = sk->has_hi;
	bool		desc;
	bool		has_tlo;
	bool		has_thi;
	Datum		tlo;
	Datum		thi;
	bool		tlo_strict;
	bool		thi_strict;
	int			nexprs = sk->neq + (sk->has_lo ? 1 : 0) + (sk->has_hi ? 1 : 0);

	sk->empty = false;
	sk->tree_lo = sk->tree_hi = NULL;
	for (i = 0; i < nexprs; i++)
	{
		int			c = Min(i, sk->neq);
		Oid			to = TupleDescAttr(s->t.desc, key->attnos[c] - 1)->atttypid;
		bool		is_lo = (i == sk->neq && sk->has_lo);
		bool		is_eq = (i < sk->neq);
		Datum		d;

		if (nulls[i])
		{
			sk->empty = true;
			return;
		}
		if (!bound_value(in[i], types[i], to, &d))
		{
			int			sign = integer_sign(in[i], types[i]);

			/* (equal to no key; past every key, or before every one) */
			if (is_eq || (is_lo && sign > 0) || (!is_lo && sign < 0))
			{
				sk->empty = true;
				return;
			}
			if (is_lo)
				has_lo = false;
			else
				has_hi = false;
			continue;
		}
		if (is_eq)
		{
			values[i] = d;
			isnull[i] = false;
		}
		else if (is_lo)
			lo = d;
		else
			hi = d;
	}
	if (sk->neq == 0 && !has_lo && !has_hi)
		return;
	desc = sk->neq < key->nkeys && (key->options[sk->neq] & INDOPTION_DESC) != 0;
	has_tlo = desc ? has_hi : has_lo;
	has_thi = desc ? has_lo : has_hi;
	tlo = desc ? hi : lo;
	thi = desc ? lo : hi;
	tlo_strict = desc ? sk->hi_strict : sk->lo_strict;
	thi_strict = desc ? sk->lo_strict : sk->hi_strict;
	if (has_tlo || sk->neq > 0)
	{
		sk->tree_lo_strict = has_tlo && tlo_strict;
		sk->tree_lo = tree_bound(sk, values, isnull, has_tlo, tlo,
								 sk->tree_lo_strict);
	}
	if (has_thi || sk->neq > 0)
	{
		sk->tree_hi_strict = has_thi && thi_strict;
		sk->tree_hi = tree_bound(sk, values, isnull, has_thi, thi,
								 !sk->tree_hi_strict);
	}
}

/* ------------------------------------------------------------------------
 * Planning
 * ------------------------------------------------------------------------
 */

/*
 * Per-tuple costs: reading a leaf item, decoding a row, and an index
 * entry's look-up of its row in the data tree.
 */
#define ITEM_COST		(cpu_tuple_cost)
#define DECODE_COST		(4 * cpu_operator_cost)
#define LOOKUP_COST		(2 * cpu_tuple_cost)

typedef struct MountPlan
{
	Mounted		s;
	KeyTree    *trees;			/* each key's, for counting; opened on use */
	double		commits;		/* (history) the commits a scan reads */
	Expr	   *commit;			/* (history) commit_hash's equality */
} MountPlan;

/* Key k's order as pathkeys (NIL where it can't be told the planner). */
static List *
key_pathkeys(PlannerInfo *root, RelOptInfo *rel, const Mounted *s,
			 const MountKey *k, bool backward)
{
	List	   *keys = NIL;

	/*
	 * (a base relation's alone: an appendrel member's equivalence members
	 * are its parent's, translated, and making new ones for it is wrong)
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return NIL;
	for (int c = 0; c < k->nkeys; c++)
	{
		Form_pg_attribute att = TupleDescAttr(s->t.desc, k->attnos[c] - 1);
		Var		   *var;
		bool		desc = (k->options[c] & INDOPTION_DESC) != 0;
		bool		nulls_first = (k->options[c] & INDOPTION_NULLS_FIRST) != 0;
		Oid			opno;
		List	   *pk;

		/*
		 * (a key ordered by another collation than its column's, or with
		 * NULLs where a sort by its operator wouldn't put them: not told)
		 */
		if (att->attcollation != k->collations[c] || nulls_first != desc)
			break;
		if (backward)
			desc = !desc;
		opno = get_opfamily_member(k->opfamilies[c], att->atttypid,
								   att->atttypid,
								   desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		if (!OidIsValid(opno))
			break;
		var = makeVar(rel->relid, k->attnos[c], att->atttypid,
					  att->atttypmod, att->attcollation, 0);
		pk = build_expression_pathkey(root, (Expr *) var, opno, rel->relids,
									  true);

		/*
		 * (as an index's pathkeys: one a constant fixes, or given already,
		 * says nothing of the order)
		 */
		if (pk != NIL &&
			!((PathKey *) linitial(pk))->pk_eclass->ec_has_const &&
			!list_member_ptr(keys, linitial(pk)))
			keys = list_concat(keys, pk);
	}
	return keys;
}

/* Whether key k's tree has key. */
static bool
tree_has(KeyTree *kt, const ProllyKey *key)
{
	StringInfoData v;
	bool		found;

	initStringInfo(&v);
	found = PROLLY_CALL(kt->c.tree, get, key_tree_env(kt), &kt->key->root, key,
						&v);
	pfree(v.data);
	return found;
}

/*
 * How many keys of key ki's tree bounds b holds, exactly, where every
 * bound is a constant and the tree counts by rank (the keys below each
 * end); -1 if not.
 */
static double
exact_rows(MountPlan *ps, int ki, const KeyBounds *b)
{
	KeyTree    *kt = &ps->trees[ki];
	Seek		sk = {0};
	Datum		values[INDEX_MAX_KEYS + 2];
	bool		nulls[INDEX_MAX_KEYS + 2];
	Oid			types[INDEX_MAX_KEYS + 2];
	int			n = 0;
	uint64		lo;
	uint64		hi;
	Expr	   *exprs[INDEX_MAX_KEYS + 2];

	for (int c = 0; c < b->neq; c++)
		exprs[n++] = b->eq[c];
	if (b->lo)
		exprs[n++] = b->lo;
	if (b->hi)
		exprs[n++] = b->hi;
	for (int i = 0; i < n; i++)
	{
		if (!IsA(exprs[i], Const))
			return -1;
		values[i] = ((Const *) exprs[i])->constvalue;
		nulls[i] = ((Const *) exprs[i])->constisnull;
		types[i] = ((Const *) exprs[i])->consttype;
	}
	if (kt->key == NULL)
		key_tree_open(kt, &ps->s, &ps->s.keys[ki], CurrentMemoryContext);
	if (!PROLLY_HAS(kt->c.tree, rank))
		return -1;
	sk.kt = kt;
	sk.neq = b->neq;
	sk.has_lo = b->lo != NULL;
	sk.has_hi = b->hi != NULL;
	sk.lo_strict = b->lo_strict;
	sk.hi_strict = b->hi_strict;
	compute_bounds(&sk, values, nulls, types);
	if (sk.empty)
		return 0;

	/*
	 * (a bound of every key column is a key itself: rank() doesn't count
	 * it, so a strict lower bound skips it and an inclusive upper takes it)
	 */
	if (sk.tree_lo)
	{
		ProllyKey	key = {(const uint8 *) sk.tree_lo->data, sk.tree_lo->len};

		lo = PROLLY_CALL(kt->c.tree, rank, key_tree_env(kt), &kt->key->root, &key);
		if (sk.tree_lo_strict && tree_has(kt, &key))
			lo++;
	}
	else
		lo = 0;
	if (sk.tree_hi)
	{
		ProllyKey	key = {(const uint8 *) sk.tree_hi->data, sk.tree_hi->len};

		hi = PROLLY_CALL(kt->c.tree, rank, key_tree_env(kt), &kt->key->root, &key);
		if (!sk.tree_hi_strict && tree_has(kt, &key))
			hi++;
	}
	else
		hi = PROLLY_CALL(kt->c.tree, count, key_tree_env(kt), &kt->key->root);
	return hi > lo ? (double) (hi - lo) : 0;
}

/* The clauses that bound key k's columns as bounds b bounds them. */
static List *
bound_clauses(RelOptInfo *rel, const Mounted *s, const MountKey *k,
			  const KeyBounds *b, List *clauses)
{
	List	   *used = NIL;
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst(lc);
		int			strat;
		Expr	   *e;

		for (int c = 0; c <= b->neq && c < k->nkeys; c++)
			if (key_clause(rel, s, k, c, ri, &strat, &e))
			{
				used = lappend(used, ri);
				break;
			}
	}
	return used;
}

static void
snapshot_get_rel_size(PlannerInfo *root, RelOptInfo *baserel,
					  Oid foreigntableid)
{
	MountPlan  *ps = palloc0(sizeof(MountPlan));
	ProllyComponents c;
	ProllyTreeEnv env;
	double		best = 0;
	List	   *best_rest = NIL;
	bool		counted = false;

	snapshot_open(foreigntableid, &ps->s);
	prolly_check_history_by_name(ps->s.source, prolly_hash_hex(&ps->s.commit),
								 true, "a snapshot mount");
	ps->trees = palloc0(sizeof(KeyTree) * Max(ps->s.nkeys, 1));
	prolly_history_components(&c);
	prolly_folded_tree_env(&env);
	ps->s.count = (double) PROLLY_CALL(c.tree, count, &env, &ps->s.t.data);
	baserel->fdw_private = ps;
	baserel->tuples = ps->s.count;
	if (ps->s.history)
	{
		List	   *rest = NIL;

		/* (an equality on commit_hash: one commit, if any) */
		foreach_node(RestrictInfo, ri, baserel->baserestrictinfo)
		{
			Expr	   *e;

			if (ps->commit == NULL && commit_clause(baserel, &ps->s, ri, &e))
				ps->commit = e;
			else
				rest = lappend(rest, ri);
		}
		/* (the commits: the mounted one's height, not walked) */
		{
			ProllyCommitInfo head;

			ps->commits = 10;
			if (ps->commit)
				ps->commits = 1;
			else if (PROLLY_CALL(refs(), commit_get, &ps->s.commit, &head) &&
					 head.has_height)
				ps->commits = Max(head.height, 1);
		}
		baserel->tuples = ps->s.count * ps->commits;
		baserel->rows = clamp_row_est(baserel->tuples *
									  clauselist_selectivity(root, rest, 0,
															 JOIN_INNER, NULL));
		return;
	}
	baserel->rows = clamp_row_est(ps->s.count *
								  clauselist_selectivity(root,
														 baserel->baserestrictinfo,
														 0, JOIN_INNER, NULL));
	/* (the fewest rows a key's constant bounds hold, counted exactly) */
	for (int i = 0; i < ps->s.nkeys; i++)
	{
		KeyBounds	b;
		double		exact;

		key_bounds(baserel, &ps->s, &ps->s.keys[i], baserel->baserestrictinfo, &b);
		if (b.neq == 0 && b.lo == NULL && b.hi == NULL)
			continue;
		if (ps->s.keys[i].unique && b.neq == ps->s.keys[i].nkeys)
		{
			best = 1;
			best_rest = NIL;
			counted = true;
			break;
		}
		exact = exact_rows(ps, i, &b);
		if (exact >= 0 && (!counted || exact < best))
		{
			best = exact;
			best_rest = list_difference_ptr(baserel->baserestrictinfo,
											bound_clauses(baserel, &ps->s,
														  &ps->s.keys[i], &b,
														  baserel->baserestrictinfo));
			counted = true;
		}
	}
	if (counted)
		baserel->rows = clamp_row_est(best *
									  clauselist_selectivity(root, best_rest, 0,
															 JOIN_INNER, NULL));
}

/* How many entries bounds b on key ki reads, of a table of count. */
static double
bounded_rows(PlannerInfo *root, RelOptInfo *rel, MountPlan *ps, int ki,
			 const KeyBounds *b, List *clauses)
{
	const Mounted *s = &ps->s;
	const MountKey *k = &s->keys[ki];
	List	   *used;
	double		exact;

	if (k->unique && b->neq == k->nkeys)
		return 1;
	if (b->neq == 0 && b->lo == NULL && b->hi == NULL)
		return Max(s->count, 1);
	exact = exact_rows(ps, ki, b);
	if (exact >= 0)
		return clamp_row_est(exact);
	used = bound_clauses(rel, s, k, b, clauses);
	return clamp_row_est(s->count * clauselist_selectivity(root, used, 0,
														   JOIN_INNER, NULL));
}

/*
 * A path's private list: direction, key and the key's index's name (for
 * EXPLAIN), then the bounds (fdw_private).
 */
static List *
path_private(const Mounted *s, const KeyBounds *b, int key, bool backward)
{
	List	   *eqs = NIL;
	const char *name = key >= 0 && s->keys[key].ix ? s->keys[key].ix->name : "";

	for (int c = 0; c < b->neq; c++)
		eqs = lappend(eqs, b->eq[c]);
	return list_make5(list_make3(makeInteger(backward ? 1 : 0),
								 makeInteger(key), makeString(pstrdup(name))),
					  eqs,
					  b->lo ? (Node *) b->lo : (Node *) makeInteger(0),
					  b->hi ? (Node *) b->hi : (Node *) makeInteger(0),
					  list_make2_int(b->lo_strict, b->hi_strict));
}

/* Whether an index's entries hold every column the query uses of rel. */
static bool
index_covers(RelOptInfo *rel, const MountKey *k)
{
	Bitmapset  *attrs = NULL;
	int			x = -1;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, rel->relid, &attrs);
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	attno = x + FirstLowInvalidHeapAttributeNumber;

		if (attno <= 0 || !k->covered[attno - 1])
			return false;
	}
	return true;
}

/*
 * The paths that scan by key ki (-1: the full scan of the data tree, in
 * the primary key's order if there is one), bounded by clauses (and
 * parameterized as ppi says, if it isn't NULL).
 */
static void
add_paths(PlannerInfo *root, RelOptInfo *rel, MountPlan *ps, int ki,
		  List *clauses, ParamPathInfo *ppi)
{
	const MountKey *k = ki >= 0 ? &ps->s.keys[ki] : NULL;
	const MountKey *ordered = k ? k : (ps->s.t.keyed ? &ps->s.keys[0] : NULL);
	KeyBounds	b;
	double		read;
	double		rows;
	Cost		startup;
	Cost		run;
	double		depth;

	memset(&b, 0, sizeof(b));
	if (k)
	{
		key_bounds(rel, &ps->s, k, clauses, &b);
		/* (bounds give a key's path; an index's order alone is a path too) */
		if (b.neq == 0 && b.lo == NULL && b.hi == NULL &&
			(ppi || k->ix == NULL || root->query_pathkeys == NIL))
			return;
	}
	read = k ? bounded_rows(root, rel, ps, ki, &b, clauses) : Max(ps->s.count, 1);
	rows = ppi ? clamp_row_est(Min(read, ppi->ppi_rows)) : Min(read, rel->rows);
	if (ppi)
		ppi->ppi_rows = rows;
	depth = ps->s.count > 1 ? ceil(log(ps->s.count) / log(64.0)) + 1 : 1;
	startup = depth * random_page_cost;
	run = read * (ITEM_COST + DECODE_COST) +
		read * cpu_operator_cost * list_length(rel->baserestrictinfo);
	if (k && k->ix && !index_covers(rel, k))
		run += read * (LOOKUP_COST + depth * cpu_operator_cost);
	for (int dir = 0; dir < 2; dir++)
	{
		List	   *pathkeys = ordered ?
			key_pathkeys(root, rel, &ps->s, ordered, dir == 1) : NIL;

		/* (backward only where it gives an order) */
		if (dir == 1 && pathkeys == NIL)
			continue;
		add_path(rel, (Path *)
				 create_foreignscan_path(root, rel, NULL, rows, 0, startup,
										 startup + run, pathkeys,
										 ppi ? ppi->ppi_req_outer : NULL,
										 NULL, NIL,
										 path_private(&ps->s, &b,
													  k ? ki : (ps->s.t.keyed ? 0 : -1),
													  dir == 1)));
	}
}

typedef struct KeyMemberArg
{
	const MountKey *k;
	Index		relid;
	int			c;				/* the key column sought */
} KeyMemberArg;

static bool
key_member(PlannerInfo *root, RelOptInfo *rel, EquivalenceClass *ec,
		   EquivalenceMember *em, void *arg)
{
	KeyMemberArg *a = arg;
	Var		   *v = (Var *) em->em_expr;

	return IsA(v, Var) && v->varno == a->relid && v->varlevelsup == 0 &&
		v->varattno == a->k->attnos[a->c];
}

/*
 * Paths parameterized by an equality of a column of key ki to another
 * relation's expression, from the join clauses and the equivalence
 * classes: a nested loop re-seeks for each outer row.
 */
static void
add_parameterized_paths(PlannerInfo *root, RelOptInfo *baserel, MountPlan *ps,
						int ki)
{
	const MountKey *k = &ps->s.keys[ki];
	List	   *ppis = NIL;
	ListCell   *lc;

	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *ri = lfirst(lc);
		int			strat;
		Expr	   *e;
		Relids		outer;

		if (!join_clause_is_movable_to(ri, baserel))
			continue;
		for (int c = 0; c < k->nkeys; c++)
			if (key_clause(baserel, &ps->s, k, c, ri, &strat, &e) &&
				strat == BTEqualStrategyNumber)
			{
				outer = bms_del_member(bms_union(ri->clause_relids,
												 baserel->lateral_relids),
									   baserel->relid);
				if (!bms_is_empty(outer))
					ppis = list_append_unique_ptr(ppis,
												  get_baserel_parampathinfo(root, baserel, outer));
				break;
			}
	}
	if (baserel->has_eclass_joins)
	{
		for (int c = 0; c < k->nkeys; c++)
		{
			KeyMemberArg a = {k, baserel->relid, c};
			List	   *clauses = generate_implied_equalities_for_column(root, baserel,
																		 key_member, &a,
																		 baserel->lateral_referencers);

			foreach(lc, clauses)
			{
				RestrictInfo *ri = lfirst(lc);
				Relids		outer;

				if (!join_clause_is_movable_to(ri, baserel))
					continue;
				outer = bms_del_member(bms_union(ri->clause_relids,
												 baserel->lateral_relids),
									   baserel->relid);
				if (!bms_is_empty(outer))
					ppis = list_append_unique_ptr(ppis,
												  get_baserel_parampathinfo(root, baserel, outer));
			}
		}
	}
	foreach(lc, ppis)
	{
		ParamPathInfo *ppi = lfirst(lc);

		add_paths(root, baserel, ps, ki,
				  list_concat_copy(baserel->baserestrictinfo, ppi->ppi_clauses),
				  ppi);
	}
}

/*
 * A history scan's path: the commits it reads (one, by an equality on
 * commit_hash), each bounded by the primary key's quals among clauses; in
 * no order.  Parameterized as ppi says, if it isn't NULL.
 */
static void
history_path(PlannerInfo *root, RelOptInfo *rel, MountPlan *ps, List *clauses,
			 ParamPathInfo *ppi)
{
	KeyBounds	b;
	Expr	   *commit = NULL;
	double		commits;
	double		per;
	double		rows;
	Cost		startup;
	Cost		run;
	List	   *priv;
	List	   *how;
	double		depth;

	memset(&b, 0, sizeof(b));
	foreach_node(RestrictInfo, ri, clauses)
		if (commit == NULL && commit_clause(rel, &ps->s, ri, &commit))
			break;
	if (ppi && commit == NULL)
		return;
	commits = commit ? 1 : ps->commits;
	if (ps->s.t.keyed)
		key_bounds(rel, &ps->s, &ps->s.keys[0], clauses, &b);
	per = (b.neq > 0 || b.lo || b.hi) && ps->s.t.keyed ?
		bounded_rows(root, rel, ps, 0, &b, clauses) :
		Max(ps->s.count, 1);
	rows = ppi ? clamp_row_est(per * clauselist_selectivity(root, ppi->ppi_clauses,
															0, JOIN_INNER, NULL)) :
		rel->rows;
	if (ppi)
		ppi->ppi_rows = rows;
	depth = ps->s.count > 1 ? ceil(log(ps->s.count) / log(64.0)) + 1 : 1;
	startup = depth * random_page_cost;
	run = commits * (per * (ITEM_COST + DECODE_COST) + depth * random_page_cost) +
		per * commits * cpu_operator_cost * list_length(clauses);
	priv = path_private(&ps->s, &b, ps->s.t.keyed ? 0 : -1, false);
	how = linitial(priv);
	how = lappend(how, makeInteger(commit != NULL));
	linitial(priv) = how;
	priv = lappend(priv, commit ? (Node *) commit : (Node *) makeInteger(0));
	add_path(rel, (Path *)
			 create_foreignscan_path(root, rel, NULL, rows, 0, startup,
									 startup + run, NIL,
									 ppi ? ppi->ppi_req_outer : NULL,
									 NULL, NIL, priv));
}

static bool
commit_member(PlannerInfo *root, RelOptInfo *rel, EquivalenceClass *ec,
			  EquivalenceMember *em, void *arg)
{
	Var		   *v = (Var *) em->em_expr;
	MountPlan  *ps = arg;

	return IsA(v, Var) && v->varno == rel->relid && v->varlevelsup == 0 &&
		v->varattno == ps->s.t.desc->natts + 1;
}

/*
 * A history scan's paths: unparameterized, and by an equality of
 * commit_hash to another relation's expression (a join to the log), each
 * outer row's commit read alone.
 */
static void
history_paths(PlannerInfo *root, RelOptInfo *rel, MountPlan *ps)
{
	List	   *ppis = NIL;
	List	   *clauses = NIL;
	ListCell   *lc;

	history_path(root, rel, ps, rel->baserestrictinfo, NULL);
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *ri = lfirst(lc);
		Expr	   *e;

		if (join_clause_is_movable_to(ri, rel) &&
			commit_clause(rel, &ps->s, ri, &e))
			clauses = lappend(clauses, ri);
	}
	if (rel->has_eclass_joins)
		clauses = list_concat(clauses,
							  generate_implied_equalities_for_column(root, rel,
																	 commit_member, ps,
																	 rel->lateral_referencers));
	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst(lc);
		Relids		outer;

		if (!join_clause_is_movable_to(ri, rel))
			continue;
		outer = bms_del_member(bms_union(ri->clause_relids, rel->lateral_relids),
							   rel->relid);
		if (!bms_is_empty(outer))
			ppis = list_append_unique_ptr(ppis,
										  get_baserel_parampathinfo(root, rel, outer));
	}
	foreach(lc, ppis)
	{
		ParamPathInfo *ppi = lfirst(lc);

		history_path(root, rel, ps,
					 list_concat_copy(rel->baserestrictinfo, ppi->ppi_clauses), ppi);
	}
}

static void
snapshot_get_paths(PlannerInfo *root, RelOptInfo *baserel,
				   Oid foreigntableid)
{
	MountPlan  *ps = baserel->fdw_private;

	if (ps->s.history)
	{
		history_paths(root, baserel, ps);
		return;
	}

	add_paths(root, baserel, ps, -1, NIL, NULL);
	for (int ki = 0; ki < ps->s.nkeys; ki++)
	{
		add_paths(root, baserel, ps, ki, baserel->baserestrictinfo, NULL);
		add_parameterized_paths(root, baserel, ps, ki);
	}
}

/* The attribute numbers the query uses of rel (0: the whole row). */
static List *
used_columns(RelOptInfo *rel, List *quals)
{
	Bitmapset  *attrs = NULL;
	List	   *cols = NIL;
	int			x = -1;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	pull_varattnos((Node *) quals, rel->relid, &attrs);
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	attno = x + FirstLowInvalidHeapAttributeNumber;

		if (attno == 0)
			return list_make1_int(0);
		if (attno > 0)
			cols = lappend_int(cols, attno);
	}
	return cols;
}

static ForeignScan *
snapshot_get_plan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
				  ForeignPath *best_path, List *tlist, List *scan_clauses,
				  Plan *outer_plan)
{
	List	   *priv = best_path->fdw_private;
	List	   *eqs = lsecond(priv);
	Node	   *lo = lthird(priv);
	Node	   *hi = lfourth(priv);
	List	   *strict = (List *) list_nth(priv, 4);
	List	   *exprs = list_copy(eqs);
	List	   *quals;

	/* (every qual a filter too: the bounds only spare reading rows) */
	quals = extract_actual_clauses(scan_clauses, false);
	if (!IsA(lo, Integer))
		exprs = lappend(exprs, lo);
	if (!IsA(hi, Integer))
		exprs = lappend(exprs, hi);
	/* (a history scan's commit_hash, last) */
	if (list_length(priv) > 5 && !IsA(list_nth(priv, 5), Integer))
		exprs = lappend(exprs, list_nth(priv, 5));
	return make_foreignscan(tlist, quals, baserel->relid, exprs,
							list_make5(linitial(priv),
									   makeInteger(list_length(eqs)),
									   list_make2_int(!IsA(lo, Integer), !IsA(hi, Integer)),
									   strict,
									   used_columns(baserel, quals)),
							NIL, NIL, outer_plan);
}

/* ------------------------------------------------------------------------
 * Scanning
 * ------------------------------------------------------------------------
 */

/* A history scan's commits and the one it reads. */
typedef struct HistoryScan
{
	bool		has_commit;		/* the last bound expression is commit_hash's */
	bool		started;
	bool		single;			/* that commit alone */
	List	   *queue;			/* (ProllyHash *) commits to read */
	HTAB	   *seen;			/* and read already */
	Datum	   *values;			/* the bound expressions' values */
	bool	   *nulls;
	MemoryContext cxt;			/* the commit's, reset for the next */
	ProllyHistoryReader *reader;	/* the table's lineage, by commit */
	ProllyNodeRef head_root;	/* the mounted commit's root */
	char	   *head_identity;	/* and identity record */
	int			head_identity_len;
	HTAB	   *reachable;		/* commits the mounted one reaches, found */
	pairingheap *frontier;		/* (Reach) found, their parents not yet */
	HTAB	   *heights;

	bool		open;			/* a commit is being read: */
	Mounted		cur;			/* its table */
	KeyTree		kt;
	Seek		seek;			/* the version's key bounds, if its key's */
	ProllyTreeCursor *cursor;
	int		   *map;			/* the version's columns in its, or -1 */
	bool	   *convert;		/* a column converted from its type there */
	FmgrInfo   *out;			/* (converted: its output function there) */
	FmgrInfo   *in;				/* (and its input function now) */
	Oid		   *ioparam;
	bool	   *warned;			/* a value didn't convert (warned once a
								 * scan) */
	bool	   *wanted;			/* its columns decoded */
	TupleTableSlot *slot;		/* a row of it */
	Datum		commit_hash;
	Datum		committer;
	Datum		commit_date;
} HistoryScan;

typedef struct MountScan
{
	Mounted		s;
	HistoryScan h;
	KeyTree		kt;				/* the tree the scan reads */
	Seek		seek;
	int			key;			/* -1: the data tree in full; else s.keys' */
	ProllyTreeCursor *cursor;
	bool		backward;
	bool		lookups;		/* (an index) rows read from the data tree */
	StringInfoData row;			/* a row looked up */
	List	   *exprs;			/* ExprStates of the bounds */
	Oid		   *types;			/* and their types */
	bool	   *wanted;			/* attributes decoded; NULL: all */
	bool		done;
	uint64		copies;			/* copies of the row in the slot still to give */
	MemoryContext cxt;
	MemoryContext boundcxt;		/* the bounds', reset at each (re)scan */

	/* work, for EXPLAIN ANALYZE */
	uint64		rows_read;
	uint64		nodes_before;
	uint64		nodes_read;
} MountScan;

static void
snapshot_begin(ForeignScanState *node, int eflags)
{
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	MountScan  *ss = palloc0(sizeof(MountScan));
	Relation	rel = node->ss.ss_currentRelation;
	List	   *priv = fs->fdw_private;
	List	   *how = linitial(priv);
	List	   *has = lthird(priv);
	List	   *strict = lfourth(priv);
	List	   *cols = (List *) list_nth(priv, 4);
	ListCell   *lc;
	int			i = 0;

	node->fdw_state = ss;
	ss->backward = intVal(linitial(how)) != 0;
	ss->key = intVal(lsecond(how));
	ss->seek.neq = intVal(lsecond(priv));
	ss->seek.has_lo = linitial_int(has);
	ss->seek.has_hi = lsecond_int(has);
	ss->seek.lo_strict = linitial_int(strict);
	ss->seek.hi_strict = lsecond_int(strict);
	ss->h.has_commit = list_length(how) > 3 && intVal(list_nth(how, 3)) != 0;
	ss->exprs = ExecInitExprList(fs->fdw_exprs, (PlanState *) node);
	ss->types = palloc(sizeof(Oid) * Max(list_length(fs->fdw_exprs), 1));
	foreach(lc, fs->fdw_exprs)
		ss->types[i++] = exprType(lfirst(lc));
	if (!(list_length(cols) == 1 && linitial_int(cols) == 0))
	{
		ss->wanted = palloc0(sizeof(bool) * Max(RelationGetDescr(rel)->natts, 1));
		foreach(lc, cols)
			ss->wanted[lfirst_int(lc) - 1] = true;
	}
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	snapshot_open(RelationGetRelid(rel), &ss->s);
	prolly_check_history_by_name(ss->s.source, prolly_hash_hex(&ss->s.commit),
								 true, "a snapshot mount");
	if (ss->key >= ss->s.nkeys)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the plan's key of \"%s\" is no longer its mount's",
						ss->s.source)));
	ss->cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly snapshot scan",
									ALLOCSET_DEFAULT_SIZES);
	ss->boundcxt = AllocSetContextCreate(ss->cxt, "prolly snapshot scan bounds",
										 ALLOCSET_SMALL_SIZES);
	key_tree_open(&ss->kt, &ss->s, ss->key >= 0 ? &ss->s.keys[ss->key] : NULL,
				  ss->cxt);
	ss->seek.kt = &ss->kt;
	if (ss->s.history)
	{
		ProllyCommitInfo head;
		MemoryContext old = MemoryContextSwitchTo(ss->cxt);

		ss->h.cxt = AllocSetContextCreate(ss->cxt, "prolly history commit",
										  ALLOCSET_DEFAULT_SIZES);
		ss->h.reader = prolly_history_reader_begin(ss->s.source,
												   "a history mount");
		ss->h.warned = palloc0(sizeof(bool) * Max(ss->s.t.desc->natts, 1));
		if (!PROLLY_CALL(refs(), commit_get, &ss->s.commit, &head))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit %s is missing",
							prolly_hash_hex(&ss->s.commit))));
		ss->h.head_root = head.root;
		ss->h.head_identity = head.identity;
		ss->h.head_identity_len = head.identity_len;
		MemoryContextSwitchTo(old);
	}
	if (ss->key >= 0 && ss->s.keys[ss->key].ix)
	{
		MountKey   *k = &ss->s.keys[ss->key];

		ss->lookups = (ss->wanted == NULL);
		for (int c = 0; !ss->lookups && c < ss->s.t.desc->natts; c++)
			ss->lookups = ss->wanted[c] && !k->covered[c];
		ss->row.data = NULL;
		initStringInfo(&ss->row);
	}
	ss->nodes_before = pgstat_prolly_tree_counters()->nodes_read;
}

/* The bounds, from the bound expressions' values now. */
static void
set_bounds(ForeignScanState *node, MountScan *ss)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			n = list_length(ss->exprs);
	Datum	   *values;
	bool	   *nulls;
	ListCell   *lc;
	int			i = 0;
	MemoryContext old;

	/* (the values, and what evaluating them makes, freed at the next) */
	MemoryContextReset(ss->boundcxt);
	ss->seek.tree_lo = ss->seek.tree_hi = NULL;
	ss->seek.empty = false;
	old = MemoryContextSwitchTo(ss->boundcxt);
	values = palloc(sizeof(Datum) * Max(n, 1));
	nulls = palloc(sizeof(bool) * Max(n, 1));
	foreach(lc, ss->exprs)
	{
		values[i] = ExecEvalExpr(lfirst(lc), econtext, &nulls[i]);
		i++;
	}
	if (ss->key >= 0)
		compute_bounds(&ss->seek, values, nulls, ss->types);
	MemoryContextSwitchTo(old);
}

static void
start_scan(ForeignScanState *node, MountScan *ss)
{
	ProllyKey	at;
	StringInfo	from;

	set_bounds(node, ss);
	ss->done = ss->seek.empty;
	ss->copies = 0;
	if (ss->done)
		return;
	if (ss->cursor == NULL)
		ss->cursor = PROLLY_CALL(ss->kt.c.tree, cursor_open, key_tree_env(&ss->kt),
								 ss->key >= 0 ? &ss->s.keys[ss->key].root :
								 &ss->s.t.data, ss->cxt);
	from = ss->backward ? ss->seek.tree_hi : ss->seek.tree_lo;
	if (from)
	{
		at.data = (const uint8 *) from->data;
		at.len = from->len;
	}
	PROLLY_CALL(ss->kt.c.tree, cursor_seek, ss->cursor, from ? &at : NULL,
				ss->backward ? BackwardScanDirection : ForwardScanDirection);
}

static int
compare_bound(MountScan *ss, const ProllyKey *key, StringInfo bound)
{
	ProllyKey	b = {(const uint8 *) bound->data, bound->len};

	return key_tree_compare(&ss->kt, key, &b);
}

/* Is key past the end the scan goes toward? */
static bool
past_end(MountScan *ss, const ProllyKey *key)
{
	StringInfo	end = ss->backward ? ss->seek.tree_lo : ss->seek.tree_hi;
	bool		strict = ss->backward ? ss->seek.tree_lo_strict : ss->seek.tree_hi_strict;
	int			c;

	if (end == NULL)
		return false;
	c = compare_bound(ss, key, end);
	if (ss->backward)
		c = -c;
	return strict ? c >= 0 : c > 0;
}

/* Is key short of a strict start, a seek having put the scan at it? */
static bool
before_start(MountScan *ss, const ProllyKey *key)
{
	StringInfo	start = ss->backward ? ss->seek.tree_hi : ss->seek.tree_lo;
	bool		strict = ss->backward ? ss->seek.tree_hi_strict : ss->seek.tree_lo_strict;
	int			c;

	if (start == NULL || !strict)
		return false;
	c = compare_bound(ss, key, start);
	if (ss->backward)
		c = -c;
	return c <= 0;
}

/* ------------------------------------------------------------------------
 * Scanning history
 * ------------------------------------------------------------------------
 */

/* Is key past a forward seek's end? */
static bool
seek_past_end(Seek *sk, const ProllyKey *key)
{
	ProllyKey	b;
	int			c;

	if (sk->tree_hi == NULL)
		return false;
	b.data = (const uint8 *) sk->tree_hi->data;
	b.len = sk->tree_hi->len;
	c = key_tree_compare(sk->kt, key, &b);
	return sk->tree_hi_strict ? c >= 0 : c > 0;
}

/* Is key short of a forward seek's strict start? */
static bool
seek_before_start(Seek *sk, const ProllyKey *key)
{
	ProllyKey	b;

	if (sk->tree_lo == NULL || !sk->tree_lo_strict)
		return false;
	b.data = (const uint8 *) sk->tree_lo->data;
	b.len = sk->tree_lo->len;
	return key_tree_compare(sk->kt, key, &b) <= 0;
}

/* The commit being read: its table, keys, cursor and column map. */
static void
history_close_commit(MountScan *ss)
{
	HistoryScan *h = &ss->h;

	if (!h->open)
		return;
	if (h->cursor)
		PROLLY_CALL(ss->kt.c.tree, cursor_close, h->cursor);
	h->cursor = NULL;
	ExecDropSingleTupleTableSlot(h->slot);
	key_tree_close(&h->kt);
	h->open = false;
	MemoryContextReset(h->cxt);
}

/*
 * Whether the commit's primary key is the mounted version's (by its
 * columns' names and types, operator families, collations and options):
 * then the version's key bounds seek in its tree too.
 */
static bool
history_same_key(const Mounted *head, const Mounted *cur, const int *map)
{
	if (!head->t.keyed || !cur->t.keyed || head->t.nkeys != cur->t.nkeys)
		return false;
	for (int k = 0; k < head->t.nkeys; k++)
	{
		int			ha = head->t.key_attnos[k] - 1;

		if (map[ha] != cur->t.key_attnos[k] - 1 ||
			TupleDescAttr(head->t.desc, ha)->atttypid !=
			TupleDescAttr(cur->t.desc, map[ha])->atttypid ||
			head->t.opfamilies[k] != cur->t.opfamilies[k] ||
			head->t.collations[k] != cur->t.collations[k] ||
			head->t.options[k] != cur->t.options[k])
			return false;
	}
	return true;
}

/*
 * The renames that read commit info's version of the mount's table as the
 * mounted commit names its columns (*renames); and its name there, the
 * mount's.  As dolt_history_table() reads a version, its table is the one
 * of that name there: a table renamed since isn't followed to its old
 * name, and the columns of a table renamed (or moved aside) are read by
 * their names there.
 */
static char *
history_version_name(MountScan *ss, const ProllyCommitInfo *info, List **renames)
{
	HistoryScan *h = &ss->h;
	ProllyValue from = {(const uint8 *) info->identity, info->identity_len};
	ProllyValue to = {(const uint8 *) h->head_identity, h->head_identity_len};
	List	   *all;
	ListCell   *lc;

	*renames = NIL;
	if ((info->identity == NULL && h->head_identity == NULL) ||
		!PROLLY_HAS(canonicalizer(), renames))
		return ss->s.source;
	all = PROLLY_CALL(canonicalizer(), renames, &info->root,
					  info->identity ? &from : NULL, &h->head_root,
					  h->head_identity ? &to : NULL);
	foreach(lc, all)
	{
		ProllyRename *r = lfirst(lc);

		if (r->column == NULL && strcmp(r->table, ss->s.source) == 0)
			return ss->s.source;
	}
	*renames = all;
	return ss->s.source;
}

/* A column of table (by its name in a commit) by its name in the mount's. */
static const char *
history_column_name(List *renames, const char *table, const char *column)
{
	ListCell   *lc;

	foreach(lc, renames)
	{
		ProllyRename *r = lfirst(lc);

		if (r->column && strcmp(r->table, table) == 0 &&
			strcmp(r->column, column) == 0)
			return r->to;
	}
	return column;
}

/*
 * Open the next commit of the history that has the table and can be
 * read here, seeking its tree by the bounds; false when there are none.
 */
static bool
history_next_commit(MountScan *ss)
{
	HistoryScan *h = &ss->h;

	history_close_commit(ss);
	while (h->queue != NIL)
	{
		ProllyHash *id = linitial(h->queue);
		ProllyCommitInfo info;
		ProllyHistoricalTable t;
		bool		found;
		const char *why;
		MemoryContext old;
		TupleDesc	hd = ss->s.t.desc;
		Mounted    *cur = &h->cur;
		char	   *hex;
		ProllyKey	at;
		char	   *email;
		char	   *xname;
		List	   *renames;

		CHECK_FOR_INTERRUPTS();
		why = NULL;
		h->queue = list_delete_first(h->queue);
		(void) hash_search(h->seen, id, HASH_FIND, &found);
		if (found)
			continue;
		(void) hash_search(h->seen, id, HASH_ENTER, &found);
		if (!PROLLY_CALL(refs(), commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit is missing"),
					 errdetail("The history of \"%s\" reaches commit %s.",
							   ss->s.source, prolly_hash_hex(id))));
		if (!h->single)
		{
			old = MemoryContextSwitchTo(ss->boundcxt);
			for (int i = 0; i < info.nparents; i++)
			{
				ProllyHash *p = palloc0(sizeof(ProllyHash));

				*p = info.parents[i];
				h->queue = lappend(h->queue, p);
			}
			MemoryContextSwitchTo(old);
		}

		old = MemoryContextSwitchTo(h->cxt);
		xname = history_version_name(ss, &info, &renames);
		found = false;
		if (xname)
			why = PROLLY_CALL(canonicalizer(), table_metadata, &info.root,
							  xname, &t, &found);
		if (!found)
		{
			/* (a commit without the table) */
			MemoryContextSwitchTo(old);
			MemoryContextReset(h->cxt);
			continue;
		}
		hex = prolly_hash_hex(&info.id);
		/* (who may read the version first: why tells of its table) */
		prolly_history_reader_check(h->reader, hex, xname);
		if (why)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("can't read the history of \"%s\": %s",
							ss->s.source, why),
					 errdetail("Commit %s holds it so.", hex)));
		memset(cur, 0, sizeof(*cur));
		cur->source = ss->s.source;
		cur->commit = info.id;
		cur->t = t;
		cur->keys = palloc0(sizeof(MountKey));
		if (t.keyed)
		{
			MountKey   *k = &cur->keys[cur->nkeys++];

			k->nkeys = t.nkeys;
			k->attnos = cur->t.key_attnos;
			k->opfamilies = cur->t.opfamilies;
			k->collations = cur->t.collations;
			k->options = cur->t.options;
			k->unique = true;
			k->root = t.data;
		}

		/*
		 * The version's columns, by identity in the commit (a column renamed
		 * since by the name it had there); others NULL.  A column whose type
		 * changed since is converted, as dolt_history_table() converts it.
		 */
		h->map = palloc(sizeof(int) * Max(hd->natts, 1));
		h->convert = palloc0(sizeof(bool) * Max(hd->natts, 1));
		h->out = palloc0(sizeof(FmgrInfo) * Max(hd->natts, 1));
		h->in = palloc0(sizeof(FmgrInfo) * Max(hd->natts, 1));
		h->ioparam = palloc0(sizeof(Oid) * Max(hd->natts, 1));
		h->wanted = palloc0(sizeof(bool) * Max(t.desc->natts, 1));
		for (int i = 0; i < hd->natts; i++)
		{
			Form_pg_attribute ha = TupleDescAttr(hd, i);

			h->map[i] = -1;
			for (int j = 0; j < t.desc->natts; j++)
			{
				Form_pg_attribute ca = TupleDescAttr(t.desc, j);

				if (strcmp(NameStr(ha->attname),
						   history_column_name(renames, xname,
											   NameStr(ca->attname))) == 0)
					h->map[i] = j;
			}
			if (h->map[i] < 0)
				continue;
			if (TupleDescAttr(t.desc, h->map[i])->atttypid != ha->atttypid ||
				TupleDescAttr(t.desc, h->map[i])->atttypmod != ha->atttypmod)
			{
				Oid			fn;
				bool		varlena;

				h->convert[i] = true;
				getTypeOutputInfo(TupleDescAttr(t.desc, h->map[i])->atttypid,
								  &fn, &varlena);
				fmgr_info_cxt(fn, &h->out[i], h->cxt);
				getTypeInputInfo(ha->atttypid, &fn, &h->ioparam[i]);
				fmgr_info_cxt(fn, &h->in[i], h->cxt);
			}
			if (ss->wanted == NULL || ss->wanted[i])
				h->wanted[h->map[i]] = true;
		}
		key_tree_open(&h->kt, cur, t.keyed ? &cur->keys[0] : NULL, h->cxt);
		h->slot = MakeSingleTupleTableSlot(t.desc, &TTSOpsVirtual);
		h->open = true;

		/* the commit's own columns */
		h->commit_hash = CStringGetTextDatum(hex);
		email = strchr(info.author, '<');
		h->committer = CStringGetTextDatum(email && email > info.author ?
										   pnstrdup(info.author,
													email - info.author - 1) :
										   info.author);
		h->commit_date = TimestampTzGetDatum(info.committed_at);

		/* the version's key bounds, where the commit's key is the version's */
		memset(&h->seek, 0, sizeof(h->seek));
		h->seek.kt = &h->kt;
		if (ss->seek.neq > 0 || ss->seek.has_lo || ss->seek.has_hi)
		{
			if (history_same_key(&ss->s, cur, h->map))
			{
				h->seek.neq = ss->seek.neq;
				h->seek.has_lo = ss->seek.has_lo;
				h->seek.has_hi = ss->seek.has_hi;
				h->seek.lo_strict = ss->seek.lo_strict;
				h->seek.hi_strict = ss->seek.hi_strict;
				compute_bounds(&h->seek, h->values, h->nulls, ss->types);
			}
		}
		MemoryContextSwitchTo(old);
		if (h->seek.empty)
		{
			history_close_commit(ss);
			continue;
		}
		h->cursor = PROLLY_CALL(ss->kt.c.tree, cursor_open, key_tree_env(&h->kt),
								&cur->t.data, h->cxt);
		if (h->seek.tree_lo)
		{
			at.data = (const uint8 *) h->seek.tree_lo->data;
			at.len = h->seek.tree_lo->len;
		}
		PROLLY_CALL(ss->kt.c.tree, cursor_seek, h->cursor,
					h->seek.tree_lo ? &at : NULL, ForwardScanDirection);
		return true;
	}
	return false;
}

/* A commit the mounted one reaches, its parents not yet looked at. */
typedef struct Reach
{
	pairingheap_node node;
	int64		height;
	int			nparents;
	ProllyHash *parents;
} Reach;

/* (the highest first: a commit reaches only commits below it) */
static int
reach_compare(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	int64		ha = pairingheap_const_container(Reach, node, a)->height;
	int64		hb = pairingheap_const_container(Reach, node, b)->height;

	return ha < hb ? -1 : ha > hb ? 1 : 0;
}

static bool
history_lookup(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	return PROLLY_CALL(refs(), commit_get, id, info);
}

/* A commit's height, its info read already. */
static int64
history_height(HistoryScan *h, const ProllyCommitInfo *info)
{
	if (info->has_height)
		return info->height;
	return prolly_history_height(&info->id, h->heights, history_lookup, NULL);
}

/* Found reachable: remembered, its parents to be looked at. */
static void
history_reach(HistoryScan *h, const ProllyHash *id)
{
	ProllyCommitInfo info;
	Reach	   *r;
	bool		found;

	(void) hash_search(h->reachable, id, HASH_ENTER, &found);
	if (found)
		return;
	if (!PROLLY_CALL(refs(), commit_get, id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit is missing"),
				 errdetail("The history reaches commit %s.",
						   prolly_hash_hex(id))));
	r = palloc(sizeof(Reach));
	r->height = history_height(h, &info);
	r->nparents = info.nparents;
	r->parents = palloc(sizeof(ProllyHash) * Max(info.nparents, 1));
	memcpy(r->parents, info.parents, sizeof(ProllyHash) * info.nparents);
	pairingheap_add(h->frontier, &r->node);
}

/*
 * Whether the mounted commit reaches commit c (or is it).  Its history is
 * walked highest first, only down to c's height (none beneath reaches
 * c), and what is found is kept for the next question: a scan reads each
 * commit once however many it asks of, and one question reads the
 * commits above c.
 */
static bool
history_reachable(MountScan *ss, const ProllyHash *c)
{
	HistoryScan *h = &ss->h;
	ProllyCommitInfo info;
	int64		height;
	bool		found;
	MemoryContext old = MemoryContextSwitchTo(ss->cxt);

	if (h->reachable == NULL)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = ss->cxt;
		h->reachable = hash_create("prolly history reachable", 256, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		h->frontier = pairingheap_allocate(reach_compare, NULL);
		h->heights = prolly_history_heights();
		history_reach(h, &ss->s.commit);
	}
	(void) hash_search(h->reachable, c, HASH_FIND, &found);
	if (!found && PROLLY_CALL(refs(), commit_get, c, &info))
	{
		height = history_height(h, &info);
		while (!found && !pairingheap_is_empty(h->frontier))
		{
			Reach	   *r = pairingheap_container(Reach, node,
												  pairingheap_first(h->frontier));

			CHECK_FOR_INTERRUPTS();
			if (r->height <= height)
				break;
			(void) pairingheap_remove_first(h->frontier);
			for (int i = 0; i < r->nparents; i++)
			{
				history_reach(h, &r->parents[i]);
				if (memcmp(&r->parents[i], c, sizeof(ProllyHash)) == 0)
					found = true;
			}
			pfree(r->parents);
			pfree(r);
		}
	}
	MemoryContextSwitchTo(old);
	return found;
}

/*
 * (Re)start a history scan: the commits it visits, from the bound
 * expressions' values now: the commit an equality on commit_hash names,
 * if the mount's commit reaches it, else every commit it reaches.
 */
static void
history_start(ForeignScanState *node, MountScan *ss)
{
	HistoryScan *h = &ss->h;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			n = list_length(ss->exprs);
	ListCell   *lc;
	int			i = 0;
	MemoryContext old;
	HASHCTL		ctl = {0};

	history_close_commit(ss);
	MemoryContextReset(ss->boundcxt);
	old = MemoryContextSwitchTo(ss->boundcxt);
	h->values = palloc(sizeof(Datum) * Max(n, 1));
	h->nulls = palloc(sizeof(bool) * Max(n, 1));
	foreach(lc, ss->exprs)
	{
		h->values[i] = ExecEvalExpr(lfirst(lc), econtext, &h->nulls[i]);
		i++;
	}
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = ss->boundcxt;
	h->seen = hash_create("prolly history commits seen", 64, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	h->queue = NIL;
	h->single = h->has_commit;
	ss->copies = 0;
	ss->done = false;
	if (h->has_commit)
	{
		ProllyHash *c = palloc0(sizeof(ProllyHash));
		char	   *hex;

		if (h->nulls[n - 1])
			ss->done = true;
		else
		{
			hex = TextDatumGetCString(h->values[n - 1]);
			if (!prolly_hash_from_hex(hex, strlen(hex), c) ||
				!history_reachable(ss, c))
				ss->done = true;
			else
				h->queue = list_make1(c);
		}
	}
	else
	{
		ProllyHash *c = palloc0(sizeof(ProllyHash));

		*c = ss->s.commit;
		h->queue = list_make1(c);
	}
	MemoryContextSwitchTo(old);
	h->started = true;
}

static TupleTableSlot *
history_iterate(ForeignScanState *node, MountScan *ss)
{
	HistoryScan *h = &ss->h;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	int			natts = ss->s.t.desc->natts;

	if (!h->started)
		history_start(node, ss);
	if (ss->copies > 0 && !TTS_EMPTY(slot))
	{
		ss->copies--;
		return slot;
	}
	ExecClearTuple(slot);
	while (!ss->done)
	{
		ProllyKey	key;
		ProllyValue value;
		ProllyValue row;
		uint64		count;

		CHECK_FOR_INTERRUPTS();
		if (!h->open && !history_next_commit(ss))
		{
			ss->done = true;
			break;
		}
		if (!PROLLY_CALL(ss->kt.c.tree, cursor_next, h->cursor, &key, &value) ||
			seek_past_end(&h->seek, &key))
		{
			history_close_commit(ss);
			continue;
		}
		if (seek_before_start(&h->seek, &key))
			continue;
		PROLLY_CALL(h->kt.c.keying, parse_value_as, h->kt.k, !h->cur.t.keyed,
					&value, &row, &count);
		PROLLY_CALL(h->kt.c.keying, decode_row_columns, h->kt.k, &row,
					h->wanted, h->slot);
		for (int i = 0; i < natts; i++)
		{
			int			j = h->map[i];

			slot->tts_isnull[i] = j < 0 || h->slot->tts_isnull[j];
			slot->tts_values[i] = j < 0 ? (Datum) 0 : h->slot->tts_values[j];
			if (h->convert[i] && !slot->tts_isnull[i])
			{
				Form_pg_attribute att = TupleDescAttr(ss->s.t.desc, i);
				ErrorSaveContext escontext = {T_ErrorSaveContext};
				char	   *str = OutputFunctionCall(&h->out[i],
													 slot->tts_values[i]);

				/* (one that doesn't convert: NULL, with a warning) */
				if (!InputFunctionCallSafe(&h->in[i], str, h->ioparam[i],
										   att->atttypmod, (Node *) &escontext,
										   &slot->tts_values[i]))
				{
					slot->tts_isnull[i] = true;
					if (!h->warned[i])
						ereport(WARNING,
								(errmsg("a value of column \"%s\" of \"%s\" doesn't convert to its type now: shown as null",
										NameStr(att->attname), ss->s.source),
								 errdetail("The value is commit %s's.",
										   TextDatumGetCString(h->commit_hash))));
					h->warned[i] = true;
				}
			}
		}
		slot->tts_values[natts] = h->commit_hash;
		slot->tts_isnull[natts] = false;
		slot->tts_values[natts + 1] = h->committer;
		slot->tts_isnull[natts + 1] = false;
		slot->tts_values[natts + 2] = h->commit_date;
		slot->tts_isnull[natts + 2] = false;
		ExecStoreVirtualTuple(slot);
		ss->rows_read++;
		ss->copies = count - 1;
		return slot;
	}
	return slot;
}

static TupleTableSlot *
snapshot_iterate(ForeignScanState *node)
{
	MountScan  *ss = node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	KeyTree    *kt = &ss->kt;
	ProllyKey	key;
	ProllyValue value;

	if (ss->s.history)
		return history_iterate(node, ss);
	if (ss->cursor == NULL && !ss->done)
		start_scan(node, ss);
	/* (the slot still holds the row, decoded once for all its copies) */
	if (ss->copies > 0 && !TTS_EMPTY(slot))
	{
		ss->copies--;
		return slot;
	}
	ExecClearTuple(slot);
	while (!ss->done)
	{
		ProllyValue row;
		uint64		count;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(kt->c.tree, cursor_next, ss->cursor, &key, &value))
		{
			ss->done = true;
			break;
		}
		if (past_end(ss, &key))
		{
			ss->done = true;
			break;
		}
		if (before_start(ss, &key))
			continue;
		PROLLY_CALL(kt->c.keying, parse_value_as, kt->k, !ss->s.t.keyed, &value,
					&row, &count);
		if (ss->lookups)
		{
			ProllyKey	idx;
			ProllyKey	dkey;
			ProllyValue dvalue;
			uint64		dcount;

			/* (the entry's row, from the data tree) */
			split_entry(&key, &idx, &dkey);
			resetStringInfo(&ss->row);
			if (!PROLLY_CALL(kt->c.tree, get, &kt->env, &ss->s.t.data, &dkey,
							 &ss->row))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a snapshot index entry of \"%s\" names no row",
								ss->s.source)));
			dvalue.data = (const uint8 *) ss->row.data;
			dvalue.len = ss->row.len;
			PROLLY_CALL(kt->c.keying, parse_value_as, kt->k, !ss->s.t.keyed,
						&dvalue, &row, &dcount);
		}
		PROLLY_CALL(kt->c.keying, decode_row_columns, kt->k, &row, ss->wanted,
					slot);
		ss->rows_read++;
		ss->copies = count - 1;
		return slot;
	}
	return slot;
}

static void
snapshot_rescan(ForeignScanState *node)
{
	MountScan  *ss = node->fdw_state;

	ss->done = false;
	if (ss->s.history)
	{
		history_start(node, ss);
		return;
	}
	start_scan(node, ss);
}

static void
snapshot_end(ForeignScanState *node)
{
	MountScan  *ss = node->fdw_state;

	if (ss == NULL || ss->cxt == NULL)
		return;
	ss->nodes_read = pgstat_prolly_tree_counters()->nodes_read - ss->nodes_before;
	if (ss->s.history)
		history_close_commit(ss);
	if (ss->cursor)
		PROLLY_CALL(ss->kt.c.tree, cursor_close, ss->cursor);
	ss->cursor = NULL;
	key_tree_close(&ss->kt);
	MemoryContextDelete(ss->cxt);
	ss->cxt = NULL;
}

static void
snapshot_explain(ForeignScanState *node, ExplainState *es)
{
	MountScan  *ss = node->fdw_state;
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	List	   *priv = fs->fdw_private;
	List	   *how = linitial(priv);
	List	   *has = lthird(priv);
	int			neq = intVal(lsecond(priv));
	const char *index = strVal(lthird(how));
	StringInfoData access;

	initStringInfo(&access);
	if (list_length(how) > 3)
		appendStringInfo(&access, "history%s, ",
						 intVal(list_nth(how, 3)) ? " of one commit" : "");
	if (neq == 0 && !linitial_int(has) && !lsecond_int(has) && index[0] == 0)
		appendStringInfoString(&access, "full");
	else
	{
		if (index[0] != 0)
			appendStringInfo(&access, "index %s", quote_identifier(index));
		else
			appendStringInfoString(&access, "primary key");
		appendStringInfo(&access, ", %d equal", neq);
		if (linitial_int(has) || lsecond_int(has))
			appendStringInfoString(&access, ", range");
		if (ss && ss->lookups)
			appendStringInfoString(&access, ", row lookups");
	}
	if (intVal(linitial(how)))
		appendStringInfoString(&access, ", backward");
	ExplainPropertyText("Snapshot Access", access.data, es);
	if (es->verbose && ss && ss->s.source)
	{
		ExplainPropertyText("Snapshot Commit", prolly_hash_hex(&ss->s.commit), es);
		ExplainPropertyText("Snapshot Table", ss->s.source, es);
	}
	if (es->analyze && ss)
	{
		uint64		nodes = ss->cxt ?
			pgstat_prolly_tree_counters()->nodes_read - ss->nodes_before :
			ss->nodes_read;

		ExplainPropertyUInteger("Rows Read", NULL, ss->rows_read, es);
		if (es->verbose)
			ExplainPropertyUInteger("Nodes Read", NULL, nodes, es);
	}
}

/*
 * prolly_snapshot_fdw_handler() returns fdw_handler
 */
Datum
prolly_snapshot_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *r = makeNode(FdwRoutine);

	r->GetForeignRelSize = snapshot_get_rel_size;
	r->GetForeignPaths = snapshot_get_paths;
	r->GetForeignPlan = snapshot_get_plan;
	r->BeginForeignScan = snapshot_begin;
	r->IterateForeignScan = snapshot_iterate;
	r->ReScanForeignScan = snapshot_rescan;
	r->EndForeignScan = snapshot_end;
	r->ExplainForeignScan = snapshot_explain;
	PG_RETURN_POINTER(r);
}
