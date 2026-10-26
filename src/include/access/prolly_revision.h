/*-------------------------------------------------------------------------
 *
 * prolly_revision.h
 *	  Revision-qualified table names: "db/ref".schema.table, a table as
 *	  another branch's working set, a tag or a commit has it, read in any
 *	  query, as Doltgres's revision databases are.
 *
 * The parser turns such a name in a FROM clause into a range table entry
 * of the function prolly_revision_table(ref, schema, table, check_as), its
 * columns
 * those the revision gives the table; the planner reads it with a custom
 * scan of its own, never through the function.  A name of the session's
 * own branch is an ordinary table name.
 *
 * src/include/access/prolly_revision.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_REVISION_H
#define PROLLY_REVISION_H

#include "nodes/pathnodes.h"
#include "parser/parse_node.h"
#include "utils/tuplestore.h"

/*
 * Is catalogname a revision of the current database ("db/ref")?  *ref is
 * then the ref, a branch, tag or commit (and ancestry), not yet looked up.
 */
extern bool prolly_revision_catalog(const char *catalogname, const char **ref);

/*
 * A catalog name a qualified name (qualified, for errors) gives where only
 * the session's tables can be named: the current database, or a revision
 * of it naming the session's branch; anything else is an error.
 */
extern void prolly_check_catalog_name(const char *catalogname,
									  const char *qualified);

/* rv, its catalog a revision ref, as a FROM clause item */
extern ParseNamespaceItem *prolly_revision_range(ParseState *pstate,
												 RangeVar *rv,
												 const char *ref);

/* Is rte a revision's table (prolly_revision_range()'s)? */
extern bool prolly_revision_rte(RangeTblEntry *rte);

/* rte's privileges are checked as userid (setRuleCheckAsUser()'s) */
extern void prolly_revision_set_check_as(RangeTblEntry *rte, Oid userid);

/* the planner's size estimate and paths for such an entry */
extern void prolly_revision_set_size(PlannerInfo *root, RelOptInfo *rel,
									 RangeTblEntry *rte);
extern void prolly_revision_set_paths(PlannerInfo *root, RelOptInfo *rel,
									  RangeTblEntry *rte);

/*
 * relid's table's rows as commit ref has them, read as dolt_at(ref,
 * NULL::t) reads them, put to store as rows of the table's descriptor; an
 * error if ref has no table of the name.
 */
extern void prolly_read_at(const char *ref, Oid relid, Tuplestorestate *store);

#endif							/* PROLLY_REVISION_H */
