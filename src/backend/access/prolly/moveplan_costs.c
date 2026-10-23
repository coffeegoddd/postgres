/*-------------------------------------------------------------------------
 *
 * moveplan_costs.c
 *	  Move planner implementation "costs": a move's tables costed from
 *	  their trees' sizes and estimated diffs.
 *
 * Each table both roots hold with the same definition and rows is left
 * out.  One whose rows differ costs the rows its diff would write, an
 * estimate from the tree's diff_estimate(), bounded in the nodes it reads,
 * so a diff too large to walk cheaply is costed high rather than walked.
 * A row written costs its heap work and an entry in each index; a row
 * loaded into a table that is new (a table made by the move, which the
 * row applier fills before building its indexes) costs much less, its
 * index entries made by a sort.  A table whose definition differs is
 * costed as one whose every row is written again; one made, as its rows
 * loaded; one dropped, as a table's catalog work.
 *
 * The figures are microseconds per row and per index entry, measured on a
 * development build: moving 100,000 changed rows of a 200,000-row table
 * took about 6.5 us a row with its primary key alone and 12.5 with two
 * more indexes; loading the 200,000 rows into a new table, 0.8 and 1.6.
 * Only their ratios matter: they rank ways of making the same working set.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/moveplan_costs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/prolly_builtins.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/fmgroids.h"
#include "utils/regproc.h"

#define ROW_WRITE_USEC		3.5		/* a row a diff writes: its heap work */
#define INDEX_WRITE_USEC	3.0		/* and each index entry it makes */
#define ROW_LOAD_USEC		0.4		/* a row loaded into a new table */
#define INDEX_LOAD_USEC		0.4		/* and its entry in each index built */
#define TABLE_USEC			2000.0	/* a table made or dropped */

/* the tree nodes one table's diff estimate may read */
#define ESTIMATE_NODES		1024

/* A root's table, as tables() visits it. */
typedef struct PlanTable
{
	char	   *name;
	ProllyNodeRef data;
	ProllyValue description;
} PlanTable;

static void
note_table(const char *name, const ProllyNodeRef *data,
		   const ProllyValue *description, const char *unreadable, void *arg)
{
	PlanTable  *t = palloc(sizeof(PlanTable));
	uint8	   *d = palloc(Max(description->len, 1));

	memcpy(d, description->data, description->len);
	t->name = pstrdup(name);
	t->data = *data;
	t->description.data = d;
	t->description.len = description->len;
	*(List **) arg = lappend(*(List **) arg, t);
}

static int
table_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((PlanTable *) lfirst(a))->name,
				  ((PlanTable *) lfirst(b))->name);
}

static List *
tables_of(const ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc;
	List	   *tables = NIL;

	prolly_database_components(&dc);
	PROLLY_CALL(dc.canonicalizer, tables, root, NULL, note_table, &tables);
	list_sort(tables, table_cmp);
	return tables;
}

/*
 * The indexes of the session branch's table name, read from pg_index
 * without opening (or locking) the table: 1, a primary key, if the
 * branch has no such table.
 */
static int
indexes_of(const char *name)
{
	Oid			relid;
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	int			n = 0;

	relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
							 NoLock, true);
	if (!OidIsValid(relid))
		return 1;
	rel = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, IndexIndrelidIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(systable_getnext(scan)))
		n++;
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return n;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static bool
same_value(const ProllyValue *a, const ProllyValue *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static void
add_move(ProllyMovePlan *plan, const char *name, ProllyTableMoveKind kind,
		 uint64 changes, uint64 rows, double cost)
{
	ProllyTableMove *m = palloc(sizeof(ProllyTableMove));

	m->name = pstrdup(name);
	m->kind = kind;
	m->changes = changes;
	m->rows = rows;
	m->cost = cost;
	plan->tables = lappend(plan->tables, m);
	plan->cost += cost;
}

static void
costs_plan(const ProllyNodeRef *from, const ProllyNodeRef *to, double bound,
		   ProllyMovePlan *plan)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	List	   *fs;
	List	   *ts;
	int			i = 0;
	int			j = 0;

	memset(plan, 0, sizeof(*plan));
	if (same_hash(&from->hash, &to->hash))
		return;
	fs = tables_of(from);
	ts = tables_of(to);
	prolly_folded_tree_env(&env);
	while ((i < list_length(fs) || j < list_length(ts)) && !plan->over)
	{
		PlanTable  *f = i < list_length(fs) ? list_nth(fs, i) : NULL;
		PlanTable  *t = j < list_length(ts) ? list_nth(ts, j) : NULL;
		int			r = f == NULL ? 1 : t == NULL ? -1 : strcmp(f->name, t->name);

		CHECK_FOR_INTERRUPTS();
		if (r < 0)
		{
			add_move(plan, f->name, PROLLY_TABLE_DROPPED, 0, 0, TABLE_USEC);
			i++;
		}
		else if (r > 0)
		{
			uint64		rows = PROLLY_CALL(tree, count, &env, &t->data);
			int			nidx = indexes_of(t->name);

			add_move(plan, t->name, PROLLY_TABLE_ADDED, rows, rows,
					 TABLE_USEC + rows * (ROW_LOAD_USEC + nidx * INDEX_LOAD_USEC));
			j++;
		}
		else
		{
			bool		alike = same_value(&f->description, &t->description);

			if (!alike || !same_hash(&f->data.hash, &t->data.hash))
			{
				uint64		rows = PROLLY_CALL(tree, count, &env, &t->data);
				int			nidx = indexes_of(t->name);
				double		row = ROW_WRITE_USEC + nidx * INDEX_WRITE_USEC;

				if (!alike)
					add_move(plan, t->name, PROLLY_TABLE_REDEFINED, rows, rows,
							 TABLE_USEC + rows * row);
				else
				{
					uint64		changes = PROLLY_CALL(tree, diff_estimate, &env,
													  &f->data, &t->data,
													  ESTIMATE_NODES);

					add_move(plan, t->name, PROLLY_TABLE_ROWS, changes, rows,
							 changes * row);
				}
			}
			i++;
			j++;
		}
		if (plan->cost > bound)
			plan->over = true;
	}
}

const ProllyMovePlannerRoutine prolly_moveplan_costs = {
	.info = {
		.iface = PROLLY_IFACE_MOVE_PLANNER,
		.impl_id = 1,
		.format_version = 1,
		.name = "costs",
		.description = "moves costed from their tables' trees and estimated diffs",
	},
	.plan = costs_plan,
};
