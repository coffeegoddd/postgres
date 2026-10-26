/*-------------------------------------------------------------------------
 *
 * prolly_revision.c
 *	  Revision-qualified table names: "db/ref".schema.table, a table as
 *	  another branch's working set, a tag or a commit has it, in any query.
 *
 * Doltgres names a revision of a database "db/ref", the ref a branch, a
 * tag or a commit (with ancestry, as HEAD~2), and a query reads any table
 * of it by that name: "db/b1".public.t, in joins, aggregates, subqueries
 * and views, a branch's table as its working set has it, uncommitted
 * changes included.  Here such a name in a FROM clause becomes a range
 * table entry of prolly_revision_table(ref, schema, table), a function
 * returning records, its columns those the revision gives the table, and
 * a custom scan of its own reads it.  A name of the session's own branch
 * is an ordinary table name, and a table outside the versioned schemas is
 * the same table on every revision.  Only reads: any other use of such a
 * name (a write, DDL, LOCK) is refused, unless it names the session's
 * branch.
 *
 * A branch other than the session's that has been used has catalog rows
 * and relations of its own (pg_class is unique on its OID and branch): its
 * working set is its copies.  The scan reads the branch's copy of the
 * table, its pages, through heap's code under the query's snapshot, as a
 * session on that branch would: rows its writers committed are seen,
 * uncommitted ones aren't, and the snapshot is one for every branch, so
 * one query reading several branches reads each as of the same moment.
 * The copy's relation cache entry is built from the rows the branch has
 * (RelationIdGetBranchRelation()), and so are its index's, which the scan
 * uses for its equalities and ranges.  The scan locks the copy as a
 * session on the branch locks it (AccessShareLock on the relation of that
 * branch), holds the branch against deletion as a session on it does
 * (pin_branch()), and is refused, as a checkout would be, where its
 * snapshot can't see the branch as it is (the refs' check_copies_read()).
 * Privileges and row security are the branch's: SELECT on its copy (or on
 * each column read), as a session on the branch would need, and a table
 * whose row security applies to the reader is refused.  The reader is the
 * role the plan checks the entry as: a view's owner (unless the view is
 * security_invoker), else the user, as for a relation.  The values a row
 * holds out of line are hash pointers to the chunk store, the same on
 * every branch.
 *
 * A branch never used has only its head: its working set is its head
 * commit.  That, a tag and a commit are read from the commit's tree as
 * dolt_at(ref, NULL::t) reads them: the table here of the name, its
 * columns as defined here, with dolt_at()'s privileges (SELECT on the
 * table here, over its lineage).
 *
 * The revision is looked up again as each scan begins, its columns
 * matched by name, so a plan or a view made earlier reads the revision as
 * it is then; a column the plan reads that the table no longer has (or
 * has with another type) is an error.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_revision.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_refs.h"
#include "access/prolly_revision.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/branch.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "varatt.h"

/* What a revision ref names. */
typedef enum RevisionKind
{
	REVISION_BRANCH,			/* a used branch: its copies */
	REVISION_COMMIT,			/* a commit (a tag, a branch with only its head) */
} RevisionKind;

typedef struct Revision
{
	const char *ref;
	RevisionKind kind;
	Oid			branch;			/* REVISION_BRANCH: the branch's OID */
} Revision;

static const ProllyRefsRoutine *
refs(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.refs;
}

/* ------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------
 */

bool
prolly_revision_catalog(const char *catalogname, const char **ref)
{
	char	   *dbname = get_database_name(MyDatabaseId);
	size_t		len;

	if (dbname == NULL)
		return false;
	len = strlen(dbname);
	if (strncmp(catalogname, dbname, len) != 0 || catalogname[len] != '/' ||
		catalogname[len + 1] == '\0')
		return false;
	*ref = catalogname + len + 1;
	return true;
}

/* Names of branches and tags matching one, case folded (ref_name()). */
typedef struct NameMatch
{
	const char *want;			/* the name sought */
	int			nfound;
	char	   *found;			/* the last found */
	bool		branch;			/* and whether it is a branch's */
} NameMatch;

static void
match_branch(const char *name, const ProllyHash *head, void *arg)
{
	NameMatch  *m = arg;

	if (pg_strcasecmp(name, m->want) == 0)
	{
		m->nfound++;
		m->found = pstrdup(name);
		m->branch = true;
	}
}

static void
match_tag(const char *name, const ProllyHash *tag, const ProllyHash *commit,
		  void *arg)
{
	NameMatch  *m = arg;

	if (pg_strcasecmp(name, m->want) == 0)
	{
		m->nfound++;
		m->found = pstrdup(name);
		m->branch = false;
	}
}

/*
 * The branch or tag name names, as Dolt matches a revision's name: a
 * branch or tag of exactly that name (a branch's first), else the one
 * branch or tag whose name differs from it only by case; more than one
 * such is an error.  NULL if none.  Branches are found by the refs'
 * case-folded index; only a name no branch or tag has exactly visits the
 * tags.
 */
static char *
ref_name(const char *name, bool *branch)
{
	const ProllyRefsRoutine *r = refs();
	ProllyBranchId id;
	ProllyHash	tag;
	NameMatch	m = {name, 0, NULL, false};

	if (PROLLY_CALL(r, branch_lookup, name, &id))
	{
		*branch = true;
		return pstrdup(name);
	}
	if (PROLLY_HAS(r, tag_lookup) && PROLLY_CALL(r, tag_lookup, name, &tag))
	{
		*branch = false;
		return pstrdup(name);
	}
	if (PROLLY_HAS(r, branches_folded))
		PROLLY_CALL(r, branches_folded, name, match_branch, &m);
	else
		PROLLY_CALL(r, branches, match_branch, &m);
	if (PROLLY_HAS(r, tags))
		PROLLY_CALL(r, tags, match_tag, &m);
	if (m.nfound > 1)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_ALIAS),
				 errmsg("revision name \"%s\" is ambiguous", name),
				 errdetail("More than one branch or tag has the name, but for its case.")));
	*branch = m.branch;
	return m.found;
}

/*
 * Look ref up: a branch or tag by its name (ref_name()), followed by any
 * ancestry (~n, ^n), else anything else the refs resolve to a commit (a
 * commit's id).  A branch without ancestry is its working set: a used
 * one's copies, one with only a head its head commit; anything else is a
 * commit.  False if ref names nothing.
 */
static bool
resolve_revision(const char *ref, Revision *rev)
{
	const ProllyRefsRoutine *r;
	ProllyBranchId branch;
	ProllyHash	commit;
	size_t		baselen = strcspn(ref, "~^");
	char	   *base;
	char	   *name;
	bool		is_branch;

	if (!database_has_versioned_schemas())
		return false;
	r = refs();
	rev->branch = InvalidOid;
	rev->kind = REVISION_COMMIT;
	base = pnstrdup(ref, baselen);
	name = ref_name(base, &is_branch);
	if (name != NULL)
	{
		rev->ref = psprintf("%s%s", name, ref + baselen);
		if (is_branch && ref[baselen] == '\0')
		{
			if (!PROLLY_CALL(r, branch_lookup, name, &branch))
				return false;
			rev->kind = OidIsValid((Oid) branch) ? REVISION_BRANCH : REVISION_COMMIT;
			rev->branch = (Oid) branch;
			return true;
		}
		return PROLLY_CALL(r, resolve, rev->ref, &commit);
	}
	rev->ref = ref;
	return PROLLY_CALL(r, resolve, ref, &commit);
}

static void
revision_missing(const char *catalogname, const char *ref, int location,
				 ParseState *pstate)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_DATABASE),
			 errmsg("database \"%s\" does not exist", catalogname),
			 errdetail("No branch, tag or commit is named \"%s\".", ref),
			 pstate ? parser_errposition(pstate, location) : 0));
}

void
prolly_check_catalog_name(const char *catalogname, const char *qualified)
{
	const char *ref;
	Revision	rev;

	if (strcmp(catalogname, get_database_name(MyDatabaseId)) == 0)
		return;
	if (prolly_revision_catalog(catalogname, &ref))
	{
		if (!resolve_revision(ref, &rev))
			revision_missing(catalogname, ref, -1, NULL);
		if (rev.kind == REVISION_BRANCH && rev.branch == MyBranch)
			return;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("tables of revision \"%s\" can only be read: %s",
						catalogname, qualified),
				 errdetail("Another branch's, a tag's or a commit's table can be named in a query's FROM clause."),
				 errhint("Check out the branch to change its tables.")));
	}
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cross-database references are not implemented: %s",
					qualified)));
}

/* ------------------------------------------------------------------------
 * A branch's copy of a table
 * ------------------------------------------------------------------------
 */

/*
 * What reading another branch's catalogs found, kept for the next query:
 * a table's OID by its name there, and that a role may read all of it (with
 * no row security).  Catalog rows read for another branch aren't kept in
 * the catalog caches, so each lookup is a catalog scan; these are flushed
 * by any change to a relation's, schema's or role's rows (on any branch:
 * invalidations name no branch), which every change that could alter what
 * they hold sends.  A table's OID found here is checked against its entry
 * once locked, as a lookup's is.
 */
typedef struct BranchNameKey
{
	Oid			branch;
	NameData	schema;
	NameData	relname;
} BranchNameKey;

typedef struct BranchNameEnt
{
	BranchNameKey key;
	Oid			relid;
} BranchNameEnt;

typedef struct BranchGrantKey
{
	Oid			branch;
	Oid			relid;
	Oid			role;
} BranchGrantKey;

static HTAB *branch_names = NULL;
static HTAB *branch_grants = NULL;

static void
branch_lookups_flush(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	void	   *ent;

	if (branch_names != NULL)
	{
		hash_seq_init(&status, branch_names);
		while ((ent = hash_seq_search(&status)) != NULL)
			hash_search(branch_names, ent, HASH_REMOVE, NULL);
	}
	if (branch_grants != NULL)
	{
		hash_seq_init(&status, branch_grants);
		while ((ent = hash_seq_search(&status)) != NULL)
			hash_search(branch_grants, ent, HASH_REMOVE, NULL);
	}
}

static void
branch_lookups_init(void)
{
	HASHCTL		ctl;

	if (branch_names != NULL)
		return;
	ctl.keysize = sizeof(BranchNameKey);
	ctl.entrysize = sizeof(BranchNameEnt);
	ctl.hcxt = CacheMemoryContext;
	branch_names = hash_create("revision table names", 64, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.keysize = sizeof(BranchGrantKey);
	ctl.entrysize = sizeof(BranchGrantKey);
	branch_grants = hash_create("revision table grants", 64, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	CacheRegisterSyscacheCallback(RELOID, branch_lookups_flush, (Datum) 0);
	CacheRegisterSyscacheCallback(NAMESPACEOID, branch_lookups_flush, (Datum) 0);
	CacheRegisterSyscacheCallback(AUTHOID, branch_lookups_flush, (Datum) 0);
	CacheRegisterSyscacheCallback(AUTHMEMMEMROLE, branch_lookups_flush, (Datum) 0);
}

/* schema.relname's OID on branch (InvalidOid: none) */
static Oid
branch_relid(Oid branch, const char *schema, const char *relname)
{
	Oid			relid = InvalidOid;
	BranchNameKey key;
	BranchNameEnt *ent;

	branch_lookups_init();
	memset(&key, 0, sizeof(key));
	key.branch = branch;
	namestrcpy(&key.schema, schema);
	namestrcpy(&key.relname, relname);
	ent = hash_search(branch_names, &key, HASH_FIND, NULL);
	if (ent != NULL)
		return ent->relid;
	CATALOG_READ_ON_BRANCH(branch,
						   {
							   Oid			nsp = get_namespace_oid(schema, true);

							   if (OidIsValid(nsp))
								   relid = get_relname_relid(relname, nsp);
						   });
	if (OidIsValid(relid))
	{
		ent = hash_search(branch_names, &key, HASH_ENTER, NULL);
		ent->relid = relid;
	}
	return relid;
}

/*
 * Is schema.relname on branch a table outside the versioned schemas (every
 * branch's, its catalog row shared), and so to be read as the session's
 * own, its locks unversioned?  False if it is versioned or not there.
 */
static bool
branch_table_shared(Oid branch, const char *schema, const char *relname)
{
	Oid			relid = branch_relid(branch, schema, relname);
	bool		shared = false;

	if (!OidIsValid(relid))
		return false;
	CATALOG_READ_ON_BRANCH(branch,
						   {
							   HeapTuple	tup = SearchSysCache1(RELOID,
																  ObjectIdGetDatum(relid));

							   if (HeapTupleIsValid(tup))
							   {
								   shared = !OidIsValid(((Form_pg_class) GETSTRUCT(tup))->relbranch);
								   ReleaseSysCache(tup);
							   }
						   });
	return shared;
}

/*
 * Branch rev's copy of schema.relname, locked as a session on the branch
 * locks it to read it, the branch held against deletion: its entry, which
 * the caller closes with RelationCloseBranchRelation().  An error if the
 * branch has no such table.
 */
static Relation
open_branch_table(const Revision *rev, const char *schema, const char *relname,
				  int location, ParseState *pstate)
{
	Oid			relid;
	Relation	rel;

	if (!pin_branch(rev->branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" can't be read now", rev->ref),
				 errdetail("It is gone, or being deleted or renamed.")));
	if (PROLLY_HAS(refs(), check_copies_read))
		PROLLY_CALL(refs(), check_copies_read, rev->ref, rev->branch);

	/* (looked up again once locked, as RangeVarGetRelid() does) */
	relid = branch_relid(rev->branch, schema, relname);
	for (;;)
	{
		Oid			again;

		if (!OidIsValid(relid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist in revision \"%s\"",
							schema, relname, rev->ref),
					 pstate ? parser_errposition(pstate, location) : 0));
		LockRelationOidOnBranch(relid, rev->branch, AccessShareLock);
		again = branch_relid(rev->branch, schema, relname);
		if (again == relid)
			break;
		relid = again;
	}
	rel = RelationIdGetBranchRelation(relid, rev->branch);
	if (rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s.%s\" does not exist in revision \"%s\"",
						schema, relname, rev->ref)));
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
	{
		RelationCloseBranchRelation(rel);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s.%s\" of revision \"%s\" is not a table",
						schema, relname, rev->ref),
				 errdetail("Only another revision's tables and materialized views can be read."),
				 pstate ? parser_errposition(pstate, location) : 0));
	}
	return rel;
}

/*
 * A table read without ONLY must have no inheritance children on branch
 * (InvalidOid: the session's, for a commit's table, which is the table
 * here): the scan reads the table alone, and a session on the revision
 * would read the children's rows too.
 */
static void
refuse_children(Relation rel, Oid branch, const char *ref, bool inh,
				int location, ParseState *pstate)
{
	bool		children = false;

	if (!inh || !rel->rd_rel->relhassubclass)
		return;
	CATALOG_READ_ON_BRANCH(branch,
						   children = find_inheritance_children(RelationGetRelid(rel),
																NoLock) != NIL);
	if (children)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("table \"%s\" of revision \"%s\" has inheritance children there",
						RelationGetRelationName(rel), ref),
				 errdetail("Another branch's table is read alone, without its children."),
				 errhint("Read it with ONLY, and each child by its own name."),
				 pstate ? parser_errposition(pstate, location) : 0));
}

/* The table here of a commit's: schema.relname, locked to read it. */
static Oid
commit_table(const char *schema, const char *relname, int location,
			 ParseState *pstate)
{
	RangeVar   *rv = makeRangeVar(pstrdup(schema), pstrdup(relname), location);

	return RangeVarGetRelid(rv, AccessShareLock, false);
}

/* ------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------
 */

static Node *
text_const(const char *s)
{
	return (Node *) makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
							  CStringGetTextDatum(s), false, false);
}

/* desc's columns as a column definition list; types must be ones here */
static List *
column_defs(TupleDesc desc, const char *ref, const char *relname,
			int location, ParseState *pstate)
{
	List	   *defs = NIL;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(att->atttypid)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("column \"%s\" of \"%s\" in revision \"%s\" has a type this branch doesn't have",
							NameStr(att->attname), relname, ref),
					 parser_errposition(pstate, location)));
		defs = lappend(defs, makeColumnDef(NameStr(att->attname),
										   att->atttypid, att->atttypmod,
										   att->attcollation));
	}
	return defs;
}

ParseNamespaceItem *
prolly_revision_range(ParseState *pstate, RangeVar *rv, const char *ref)
{
	Revision	rev;
	List	   *defs;
	FuncExpr   *fexpr;
	RangeFunction *rf;
	bool		plain = false;

	/*
	 * An identifier this long may have been cut short (at a character's
	 * boundary, so up to a character's bytes short of NAMEDATALEN - 1):
	 * refused.
	 */
	if (strlen(rv->catalogname) >= NAMEDATALEN - pg_database_encoding_max_length())
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("revision-qualified name \"%s\" is too long", rv->catalogname),
				 errdetail("A database's name, \"/\" and a revision's name are at most %d bytes together.",
						   NAMEDATALEN - pg_database_encoding_max_length() - 1),
				 parser_errposition(pstate, rv->location)));
	if (!resolve_revision(ref, &rev))
		revision_missing(rv->catalogname, ref, rv->location, pstate);
	if (rev.kind == REVISION_BRANCH && rev.branch == MyBranch)
		plain = true;
	else if (rev.kind == REVISION_BRANCH &&
			 branch_table_shared(rev.branch, rv->schemaname, rv->relname))
		plain = true;			/* (outside the versioned schemas: every branch's) */
	else if (rev.kind == REVISION_BRANCH)
	{
		Relation	rel = open_branch_table(&rev, rv->schemaname, rv->relname,
											rv->location, pstate);

		PG_TRY();
		{
			refuse_children(rel, rev.branch, ref, rv->inh, rv->location, pstate);
		}
		PG_CATCH();
		{
			RelationCloseBranchRelation(rel);
			PG_RE_THROW();
		}
		PG_END_TRY();
		defs = column_defs(RelationGetDescr(rel), ref, rv->relname,
						   rv->location, pstate);
		RelationCloseBranchRelation(rel);
	}
	else
	{
		Oid			relid = commit_table(rv->schemaname, rv->relname,
										 rv->location, pstate);
		Relation	rel = table_open(relid, NoLock);

		plain = rel->rd_rel->relam != PROLLY_TABLE_AM_OID;
		if (!plain && rel->rd_rel->relkind != RELKIND_RELATION)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s.%s\" of revision \"%s\" is not a table",
							rv->schemaname, rv->relname, ref),
					 parser_errposition(pstate, rv->location)));
		/*
		 * (a commit's table is read as dolt_at() reads it: the table here,
		 * so its children are the table's here, read without ONLY refused)
		 */
		if (!plain)
			refuse_children(rel, InvalidOid, ref, rv->inh, rv->location, pstate);
		defs = plain ? NIL : column_defs(RelationGetDescr(rel), ref,
										 rv->relname, rv->location, pstate);
		table_close(rel, NoLock);
	}

	if (plain)
	{
		RangeVar   *same = copyObject(rv);

		same->catalogname = NULL;
		return addRangeTableEntry(pstate, same, same->alias, same->inh, true);
	}

	fexpr = makeFuncExpr(F_PROLLY_REVISION_TABLE, RECORDOID,
						 list_make5(text_const(ref), text_const(rv->schemaname),
									text_const(rv->relname),
									makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
											  ObjectIdGetDatum(InvalidOid),
											  false, true),
									makeBoolConst(rv->inh, false)),
						 InvalidOid, InvalidOid, COERCE_SQL_SYNTAX);
	fexpr->funcretset = true;
	fexpr->location = rv->location;
	rf = makeNode(RangeFunction);
	rf->alias = rv->alias ? rv->alias : makeAlias(rv->relname, NIL);
	rf->coldeflist = defs;
	return addRangeTableEntryForFunction(pstate,
										 list_make1(pstrdup(rv->relname)),
										 list_make1(fexpr),
										 list_make1(defs),
										 rf, false, true);
}

/* ------------------------------------------------------------------------
 * Reading a revision's table
 * ------------------------------------------------------------------------
 */

/* One of a plan's index keys: an index column compared to an expression. */
typedef struct RevisionKey
{
	AttrNumber	column;			/* the index's column (from 1) */
	StrategyNumber strategy;
	RegProcedure proc;			/* the comparison's function */
	Oid			subtype;		/* its right side's type */
	Oid			collation;
	ExprState  *value;			/* the right side */
} RevisionKey;

/* A scan of a revision's table, by the scan, or the function. */
typedef struct RevisionReader
{
	Revision	rev;
	char	   *schema;
	char	   *relname;
	Snapshot	snapshot;
	Oid			check_as;		/* whose privileges (InvalidOid: the user's) */
	bool		inh;			/* not ONLY: its inheritance children too */

	/* the query's columns: names and types, in its order */
	int			ncols;
	char	  **colnames;
	Oid		   *coltypes;
	AttrNumber *source;			/* each one's in the rows read (0: unread) */

	/* a branch's copy: the relation, its index if scanned by one */
	Relation	heap;
	Relation	index;
	TableScanDesc hscan;
	IndexScanDesc iscan;
	int			nkeys;
	RevisionKey *keys;
	ScanKey		scankeys;
	bool		keys_ready;

	/* a commit's table: its rows, read once */
	Oid			relid;
	Tuplestorestate *store;

	TupleTableSlot *slot;		/* the row read */
	bool		started;
} RevisionReader;

/*
 * Match the query's columns used (used[i]: the i'th is read) to desc's,
 * by name; an error if one is missing, or of another type, since planned.
 */
static void
reader_map(RevisionReader *rr, TupleDesc desc, const bool *used)
{
	rr->source = palloc0_array(AttrNumber, rr->ncols);
	for (int i = 0; i < rr->ncols; i++)
	{
		int			j;

		if (!used[i])
			continue;
		for (j = 0; j < desc->natts; j++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, j);

			if (!att->attisdropped &&
				strcmp(NameStr(att->attname), rr->colnames[i]) == 0)
				break;
		}
		if (j == desc->natts ||
			TupleDescAttr(desc, j)->atttypid != rr->coltypes[i])
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("table \"%s.%s\" of revision \"%s\" changed since the query was planned",
							rr->schema, rr->relname, rr->rev.ref),
					 j == desc->natts ?
					 errdetail("It has no column \"%s\" now.", rr->colnames[i]) :
					 errdetail("Its column \"%s\" is of type %s now.",
							   rr->colnames[i],
							   format_type_be(TupleDescAttr(desc, j)->atttypid))));
		rr->source[i] = j + 1;
	}
}

/*
 * May the user read the columns used of the branch's copy (its grants, as
 * a session on the branch has them)?  An error if not, or if its row
 * security applies to the user.
 */
static void
check_branch_privileges(RevisionReader *rr, const bool *used)
{
	Oid			relid = RelationGetRelid(rr->heap);
	Oid			user = OidIsValid(rr->check_as) ? rr->check_as : GetUserId();
	bool		allowed = true;
	bool		usage = true;
	int			rls = RLS_NONE;
	BranchGrantKey key;

	branch_lookups_init();
	key.branch = rr->rev.branch;
	key.relid = relid;
	key.role = user;
	if (hash_search(branch_grants, &key, HASH_FIND, NULL) != NULL)
		return;
	CATALOG_READ_ON_BRANCH(rr->rev.branch,
						   {
							   bool		table_ok;

							   /* (its schema's USAGE, as naming it on the branch takes) */
							   if (object_aclcheck(NamespaceRelationId,
												   rr->heap->rd_rel->relnamespace,
												   user, ACL_USAGE) != ACLCHECK_OK)
								   usage = false;
							   table_ok = pg_class_aclcheck(relid, user, ACL_SELECT) == ACLCHECK_OK;
							   if (!table_ok)
							   {
								   bool		any = false;

								   for (int i = 0; i < rr->ncols && allowed; i++)
								   {
									   if (!used[i])
										   continue;
									   any = true;
									   if (pg_attribute_aclcheck(relid, rr->source[i], user,
																 ACL_SELECT) != ACLCHECK_OK)
										   allowed = false;
								   }
								   if (!any &&
									   pg_attribute_aclcheck_all(relid, user, ACL_SELECT,
																 ACLMASK_ANY) != ACLCHECK_OK)
									   allowed = false;
							   }
							   if (rr->heap->rd_rel->relrowsecurity)
								   rls = check_enable_rls(relid, rr->check_as, true);
							   else if (table_ok && usage)
								   (void) hash_search(branch_grants, &key, HASH_ENTER, NULL);
						   });
	if (!usage)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for schema %s of revision \"%s\"",
						rr->schema, rr->rev.ref),
				 errdetail("Reading another branch's table takes the branch's grants on it.")));
	if (!allowed)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for table %s of revision \"%s\"",
						rr->relname, rr->rev.ref),
				 errdetail("Reading another branch's table takes the branch's grants on it.")));
	if (rls == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("table %s of revision \"%s\" has row security that applies to the reader",
						rr->relname, rr->rev.ref),
				 errdetail("Another branch's table is read only where its row security doesn't apply.")));
}

/*
 * Begin reading the revision's table: look the revision up, open, lock and
 * check what it reads.  indexid (InvalidOid: none) is the index the plan
 * scans by, if the copy still has it.
 */
static void
reader_open(RevisionReader *rr, const char *ref, const char *schema,
			const char *relname, const bool *used, Oid indexid)
{
	rr->schema = pstrdup(schema);
	rr->relname = pstrdup(relname);
	if (!resolve_revision(ref, &rr->rev))
		revision_missing(psprintf("%s/%s", get_database_name(MyDatabaseId), ref),
						 ref, -1, NULL);
	if (rr->rev.kind == REVISION_BRANCH)
	{
		rr->heap = open_branch_table(&rr->rev, schema, relname, -1, NULL);
		reader_map(rr, RelationGetDescr(rr->heap), used);
		check_branch_privileges(rr, used);
		refuse_children(rr->heap, rr->rev.branch, rr->rev.ref, rr->inh, -1, NULL);
		if (OidIsValid(indexid) &&
			list_member_oid(RelationGetIndexList(rr->heap), indexid))
		{
			LockRelationOidOnBranch(indexid, rr->rev.branch, AccessShareLock);
			rr->index = RelationIdGetBranchRelation(indexid, rr->rev.branch);
		}
		rr->slot = MakeSingleTupleTableSlot(RelationGetDescr(rr->heap),
											table_slot_callbacks(rr->heap));
	}
	else
	{
		Relation	rel;

		rr->relid = commit_table(schema, relname, -1, NULL);
		rel = table_open(rr->relid, NoLock);
		refuse_children(rel, InvalidOid, rr->rev.ref, rr->inh, -1, NULL);
		reader_map(rr, RelationGetDescr(rel), used);
		rr->slot = MakeSingleTupleTableSlot(CreateTupleDescCopy(RelationGetDescr(rel)),
											&TTSOpsMinimalTuple);
		table_close(rel, NoLock);
	}
}

static void
reader_set_keys(RevisionReader *rr, ExprContext *econtext)
{
	for (int i = 0; i < rr->nkeys; i++)
	{
		RevisionKey *k = &rr->keys[i];
		bool		isnull;
		Datum		value;

		value = ExecEvalExprSwitchContext(k->value, econtext, &isnull);
		ScanKeyEntryInitialize(&rr->scankeys[i], isnull ? SK_ISNULL : 0,
							   k->column, k->strategy, k->subtype,
							   k->collation, k->proc, value);
	}
	rr->keys_ready = true;
}

/*
 * A commit's rows, read with the privileges of the role the plan checks
 * them as (a view's owner), as its history functions check the user's.
 */
static void
read_at_as(RevisionReader *rr)
{
	Oid			saved_user;
	int			saved_context;

	if (!OidIsValid(rr->check_as) || rr->check_as == GetUserId())
	{
		prolly_read_at(rr->rev.ref, rr->relid, rr->store);
		return;
	}
	GetUserIdAndSecContext(&saved_user, &saved_context);
	SetUserIdAndSecContext(rr->check_as,
						   saved_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		prolly_read_at(rr->rev.ref, rr->relid, rr->store);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(saved_user, saved_context);
	}
	PG_END_TRY();
}

/* The next row into rr->slot; false at the end. */
static bool
reader_next(RevisionReader *rr, ExprContext *econtext)
{
	if (rr->rev.kind == REVISION_COMMIT)
	{
		if (rr->store == NULL)
		{
			rr->store = tuplestore_begin_heap(false, false, work_mem);
			read_at_as(rr);
		}
		return tuplestore_gettupleslot(rr->store, true, false, rr->slot);
	}
	if (rr->index != NULL)
	{
		if (rr->iscan == NULL)
		{
			rr->iscan = index_beginscan(rr->heap, rr->index, false,
										rr->snapshot, NULL, rr->nkeys, 0,
										SO_HINT_REL_READ_ONLY);
			rr->keys_ready = false;
		}
		if (!rr->keys_ready)
		{
			reader_set_keys(rr, econtext);
			index_rescan(rr->iscan, rr->scankeys, rr->nkeys, NULL, 0);
		}
		return table_index_getnext_slot(rr->iscan, ForwardScanDirection, rr->slot);
	}
	if (rr->hscan == NULL)
		rr->hscan = table_beginscan(rr->heap, rr->snapshot, 0, NULL,
									SO_HINT_REL_READ_ONLY);
	return table_scan_getnextslot(rr->hscan, ForwardScanDirection, rr->slot);
}

static void
reader_rescan(RevisionReader *rr)
{
	if (rr->store != NULL)
		tuplestore_rescan(rr->store);
	rr->keys_ready = false;
	if (rr->hscan != NULL)
		table_rescan(rr->hscan, NULL);
}

static void
reader_close(RevisionReader *rr)
{
	if (rr->iscan != NULL)
		index_endscan(rr->iscan);
	if (rr->hscan != NULL)
		table_endscan(rr->hscan);
	if (rr->slot != NULL)
		ExecDropSingleTupleTableSlot(rr->slot);
	if (rr->index != NULL)
		RelationCloseBranchRelation(rr->index);
	if (rr->heap != NULL)
		RelationCloseBranchRelation(rr->heap);
	if (rr->store != NULL)
		tuplestore_end(rr->store);
	rr->iscan = NULL;
	rr->hscan = NULL;
	rr->slot = NULL;
	rr->index = NULL;
	rr->heap = NULL;
	rr->store = NULL;
}

/*
 * The i'th query column of the row read.  A value out of line in the
 * copy's TOAST relation (not a hash pointer) would be read from the
 * session branch's: refused.
 */
static Datum
reader_value(RevisionReader *rr, int i, bool *isnull)
{
	Datum		d = slot_getattr(rr->slot, rr->source[i], isnull);

	if (!*isnull &&
		TupleDescAttr(rr->slot->tts_tupleDescriptor, rr->source[i] - 1)->attlen == -1)
	{
		varlena    *v = (varlena *) DatumGetPointer(d);

		if (VARATT_IS_EXTERNAL_ONDISK(v) && !VARATT_IS_EXTERNAL_ONDISK_HASH(v))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" of table %s of revision \"%s\" holds a value in a TOAST relation",
							rr->colnames[i], rr->relname, rr->rev.ref),
					 errdetail("Only values out of line in the chunk store can be read from another branch.")));
	}
	return d;
}

/* ------------------------------------------------------------------------
 * The planner
 * ------------------------------------------------------------------------
 */

/* A btree index of a branch's copy the planner may scan by. */
typedef struct RevisionIndex
{
	Oid			indexid;
	char	   *name;
	int			ncolumns;		/* key columns */
	AttrNumber *heapattno;		/* each one's column of the table */
	Oid		   *opfamily;
	Oid		   *opcintype;
	Oid		   *collation;
	bool		unique;
	BlockNumber pages;
} RevisionIndex;

/* What the size estimate found, for the paths. */
typedef struct RevisionPlan
{
	Revision	rev;
	char	   *schema;
	char	   *relname;
	AttrNumber *heapattno;		/* each query column's in the copy (0: none) */
	List	   *indexes;		/* RevisionIndex */
} RevisionPlan;

/* the call's arguments */
static void
rte_args(RangeTblEntry *rte, char **ref, char **schema, char **relname,
		 Oid *check_as, bool *inh)
{
	RangeTblFunction *rtfunc = linitial(rte->functions);
	List	   *args = ((FuncExpr *) rtfunc->funcexpr)->args;

	*ref = TextDatumGetCString(((Const *) linitial(args))->constvalue);
	*schema = TextDatumGetCString(((Const *) lsecond(args))->constvalue);
	*relname = TextDatumGetCString(((Const *) lthird(args))->constvalue);
	if (check_as)
		*check_as = DatumGetObjectId(((Const *) lfourth(args))->constvalue);
	if (inh)
		*inh = DatumGetBool(((Const *) list_nth(args, 4))->constvalue);
}

/* rte's name as written, "db/ref".schema.table, the current database's */
char *
prolly_revision_rte_name(RangeTblEntry *rte)
{
	char	   *ref;
	char	   *schema;
	char	   *relname;
	bool		inh;

	rte_args(rte, &ref, &schema, &relname, NULL, &inh);
	return psprintf("%s%s.%s.%s", inh ? "" : "ONLY ",
					quote_identifier(psprintf("%s/%s",
											  get_database_name(MyDatabaseId),
											  ref)),
					quote_identifier(schema), quote_identifier(relname));
}

/*
 * A view's (or rule's) revision read checks its privileges as userid, as
 * setRuleCheckAsUser() makes its relations' be checked: the role is the
 * call's last argument (InvalidOid: the user's).
 */
void
prolly_revision_set_check_as(RangeTblEntry *rte, Oid userid)
{
	RangeTblFunction *rtfunc;
	Const	   *c;

	if (!prolly_revision_rte(rte))
		return;
	rtfunc = linitial(rte->functions);
	c = (Const *) lfourth(((FuncExpr *) rtfunc->funcexpr)->args);
	c->constvalue = ObjectIdGetDatum(userid);
}

bool
prolly_revision_rte(RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc;
	FuncExpr   *f;
	ListCell   *lc;

	if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1 ||
		rte->funcordinality)
		return false;
	rtfunc = linitial(rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return false;
	f = (FuncExpr *) rtfunc->funcexpr;
	/* (the parser's own call: no other is accepted, see revision_call()) */
	if (f->funcid != F_PROLLY_REVISION_TABLE || list_length(f->args) != 5 ||
		f->funcformat != COERCE_SQL_SYNTAX)
		return false;
	foreach(lc, f->args)
	{
		if (!IsA(lfirst(lc), Const) || ((Const *) lfirst(lc))->constisnull)
			return false;
	}
	return true;
}

static List *
copy_indexes(Relation heap, Oid branch)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(heap))
	{
		Oid			indexid = lfirst_oid(lc);
		Relation	ix;
		RevisionIndex *ri;
		bool		usable;

		LockRelationOidOnBranch(indexid, branch, AccessShareLock);
		ix = RelationIdGetBranchRelation(indexid, branch);
		if (ix == NULL)
			continue;
		usable = ix->rd_rel->relam == BTREE_AM_OID &&
			ix->rd_index->indisvalid && ix->rd_index->indisready &&
			heap_attisnull(ix->rd_indextuple, Anum_pg_index_indpred, NULL);
		for (int i = 0; usable && i < IndexRelationGetNumberOfKeyAttributes(ix); i++)
			usable = ix->rd_index->indkey.values[i] > 0;
		if (usable)
		{
			int			n = IndexRelationGetNumberOfKeyAttributes(ix);

			ri = palloc0_object(RevisionIndex);
			ri->indexid = indexid;
			ri->name = pstrdup(RelationGetRelationName(ix));
			ri->ncolumns = n;
			ri->heapattno = palloc_array(AttrNumber, n);
			ri->opfamily = palloc_array(Oid, n);
			ri->opcintype = palloc_array(Oid, n);
			ri->collation = palloc_array(Oid, n);
			for (int i = 0; i < n; i++)
			{
				ri->heapattno[i] = ix->rd_index->indkey.values[i];
				ri->opfamily[i] = ix->rd_opfamily[i];
				ri->opcintype[i] = ix->rd_opcintype[i];
				ri->collation[i] = ix->rd_indcollation[i];
			}
			ri->unique = ix->rd_index->indisunique;
			ri->pages = RelationGetNumberOfBlocks(ix);
			out = lappend(out, ri);
		}
		RelationCloseBranchRelation(ix);
	}
	return out;
}

void
prolly_revision_set_size(PlannerInfo *root, RelOptInfo *rel,
						 RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc = linitial(rte->functions);
	RevisionPlan *rp = palloc0_object(RevisionPlan);
	char	   *ref;
	BlockNumber pages = 0;
	double		tuples = 0;
	double		allvisfrac = 0;
	int			ncols = list_length(rtfunc->funccolnames);

	rte_args(rte, &ref, &rp->schema, &rp->relname, NULL, NULL);
	rp->heapattno = palloc0_array(AttrNumber, ncols);
	if (!resolve_revision(ref, &rp->rev))
		revision_missing(psprintf("%s/%s", get_database_name(MyDatabaseId), ref),
						 ref, -1, NULL);
	if (rp->rev.kind == REVISION_BRANCH)
	{
		Relation	heap = open_branch_table(&rp->rev, rp->schema, rp->relname,
											 -1, NULL);
		TupleDesc	desc = RelationGetDescr(heap);
		ListCell   *lc;
		int			i = 0;

		CATALOG_READ_ON_BRANCH(rp->rev.branch,
							   estimate_rel_size(heap, NULL, &pages, &tuples,
												 &allvisfrac));
		foreach(lc, rtfunc->funccolnames)
		{
			for (int j = 0; j < desc->natts; j++)
			{
				Form_pg_attribute att = TupleDescAttr(desc, j);

				if (!att->attisdropped &&
					strcmp(NameStr(att->attname), strVal(lfirst(lc))) == 0 &&
					att->atttypid == list_nth_oid(rtfunc->funccoltypes, i))
					rp->heapattno[i] = j + 1;
			}
			i++;
		}
		rp->indexes = copy_indexes(heap, rp->rev.branch);
		RelationCloseBranchRelation(heap);
	}
	else
	{
		Oid			relid = commit_table(rp->schema, rp->relname, -1, NULL);
		Relation	heap = table_open(relid, NoLock);

		estimate_rel_size(heap, NULL, &pages, &tuples, &allvisfrac);
		table_close(heap, NoLock);
	}
	rel->pages = pages;
	rel->tuples = tuples;
	rel->allvisfrac = allvisfrac;
	rel->fdw_private = rp;
	set_baserel_size_estimates(root, rel);
}

/*
 * A restriction clause's comparison of index ri's column c to something
 * the scan can compute before reading rows: its key, the clause put as
 * column op value (the operator commuted if need be); NULL if none.
 */
static OpExpr *
key_clause(RelOptInfo *rel, RevisionPlan *rp, RevisionIndex *ri, int c,
		   RestrictInfo *rinfo, int *strategy, Oid *lefttype, Oid *righttype)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Oid			opno;

	if (rinfo->pseudoconstant || !IsA(rinfo->clause, OpExpr))
		return NULL;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2)
		return NULL;
	left = linitial(op->args);
	right = lsecond(op->args);
	opno = op->opno;
	if (IsA(right, RelabelType))
		right = (Node *) ((RelabelType *) right)->arg;
	if (IsA(left, RelabelType))
		left = (Node *) ((RelabelType *) left)->arg;
	if (!(IsA(left, Var) && ((Var *) left)->varno == rel->relid))
	{
		Node	   *t = left;

		left = right;
		right = t;
		opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return NULL;
	}
	if (!IsA(left, Var) || ((Var *) left)->varno != rel->relid ||
		((Var *) left)->varlevelsup != 0 || ((Var *) left)->varattno <= 0 ||
		rp->heapattno[((Var *) left)->varattno - 1] != ri->heapattno[c])
		return NULL;
	if (bms_is_member(rel->relid, pull_varnos(NULL, right)) ||
		contain_volatile_functions(right))
		return NULL;
	if (!op_in_opfamily(opno, ri->opfamily[c]))
		return NULL;
	get_op_opfamily_properties(opno, ri->opfamily[c], false, strategy,
							   lefttype, righttype);
	if (*lefttype != ri->opcintype[c])
		return NULL;
	if (OidIsValid(ri->collation[c]) && op->inputcollid != ri->collation[c])
		return NULL;
	return (OpExpr *) make_opclause(opno, BOOLOID, false, (Expr *) left,
									(Expr *) right, op->opcollid,
									op->inputcollid);
}

static const CustomPathMethods revision_path_methods;
static const CustomScanMethods revision_scan_methods;

static CustomPath *
revision_path(RelOptInfo *rel, Cost startup, Cost total, double rows,
			  List *private)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.startup_cost = startup;
	cp->path.total_cost = total;
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = NIL;
	cp->custom_private = private;
	cp->methods = &revision_path_methods;
	return cp;
}

/*
 * The paths: a sequential scan, and for a branch's copy a scan of each
 * btree index whose leading columns the restriction clauses compare to
 * values known before the scan (equalities, then one more column's
 * comparison).  The clauses all stay the scan's filter too.
 */
void
prolly_revision_set_paths(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte)
{
	RevisionPlan *rp = rel->fdw_private;
	QualCost	qcost;
	double		tuples = rel->tuples;
	Cost		cpu_per_tuple;
	Cost		run;
	ListCell   *lc;

	cost_qual_eval(&qcost, rel->baserestrictinfo, root);
	cpu_per_tuple = cpu_tuple_cost + qcost.per_tuple;
	if (rp->rev.kind == REVISION_COMMIT)
	{
		/* (rows decoded from the commit's tree, all of them, then kept) */
		run = seq_page_cost * rel->pages + 4 * cpu_per_tuple * tuples;
	}
	else
		run = seq_page_cost * rel->pages + cpu_per_tuple * tuples;
	add_path(rel, (Path *) revision_path(rel, qcost.startup,
										 qcost.startup + run, rel->rows,
										 list_make1(NIL)));

	foreach(lc, rp->indexes)
	{
		RevisionIndex *ri = lfirst(lc);
		List	   *keys = NIL;
		List	   *clauses = NIL;
		bool		all_equal = true;
		int			c;
		Selectivity sel;
		double		fetched;
		Cost		cost;

		for (c = 0; c < ri->ncolumns; c++)
		{
			bool		equal = false;
			bool		found = false;
			ListCell   *lc2;

			foreach(lc2, rel->baserestrictinfo)
			{
				RestrictInfo *rinfo = lfirst(lc2);
				int			strategy;
				Oid			lefttype;
				Oid			righttype;
				OpExpr	   *clause;

				clause = key_clause(rel, rp, ri, c, rinfo, &strategy,
									&lefttype, &righttype);
				if (clause == NULL)
					continue;
				found = true;
				equal |= strategy == BTEqualStrategyNumber;
				keys = lappend(keys,
							   list_make5(makeInteger(c + 1),
										  makeInteger(strategy),
										  makeInteger((int) righttype),
										  makeInteger((int) ri->collation[c]),
										  clause));
				clauses = lappend(clauses, rinfo);
			}
			if (!found || !equal)
			{
				all_equal = false;
				break;
			}
		}
		if (keys == NIL)
			continue;
		sel = clauselist_selectivity(root, clauses, rel->relid, JOIN_INNER,
									 NULL);
		fetched = (ri->unique && all_equal) ? 1 : clamp_row_est(sel * tuples);
		cost = random_page_cost * (1 + Min(fetched, Max(rel->pages, 1))) +
			(cpu_index_tuple_cost + cpu_per_tuple) * fetched +
			cpu_operator_cost * list_length(keys);
		add_path(rel, (Path *) revision_path(rel, qcost.startup,
											 qcost.startup + cost,
											 rel->rows,
											 list_make3(keys,
														makeInteger((int) ri->indexid),
														makeString(ri->name))));
	}
}

/*
 * The plan: no scan relation (the entry is a function's), its scan tuple
 * the Vars of the entry the scan computes (custom_scan_tlist); its index
 * keys as clauses in custom_exprs.  custom_private: the ref, schema and
 * table, the query's column names and types, which of them the query
 * reads, then the index's OID, name and each key's column, strategy, right
 * side's type and collation.
 */
static Plan *
revision_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			  List *tlist, List *clauses, List *custom_plans)
{
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	RangeTblFunction *rtfunc = linitial(rte->functions);
	CustomScan *cs = makeNode(CustomScan);
	List	   *quals = extract_actual_clauses(clauses, false);
	List	   *vars;
	List	   *scan_tlist = NIL;
	List	   *used = NIL;
	List	   *keyinfo = NIL;
	List	   *keyclauses = NIL;
	Bitmapset  *attnos = NULL;
	bool		whole = false;
	char	   *ref;
	char	   *schema;
	char	   *relname;
	Oid			check_as;
	bool		inh;
	int			ncols = list_length(rtfunc->funccolnames);
	ListCell   *lc;
	int			i;
	List	   *path_private = best_path->custom_private;
	Oid			indexid = InvalidOid;
	char	   *indexname = NULL;

	if (list_length(path_private) == 3)
	{
		foreach(lc, (List *) linitial(path_private))
		{
			List	   *k = lfirst(lc);

			keyinfo = lappend(keyinfo, list_make4(linitial(k), lsecond(k),
												  lthird(k), lfourth(k)));
			keyclauses = lappend(keyclauses, list_nth(k, 4));
		}
		indexid = (Oid) intVal(lsecond(path_private));
		indexname = strVal(lthird(path_private));
	}

	/* the columns the query reads, for its privileges */
	vars = pull_var_clause((Node *) list_concat_copy(rel->reltarget->exprs, quals),
						   PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
						   PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = lfirst(lc);

		if (IsA(v, Var) && v->varno == rel->relid)
		{
			if (v->varattno == 0)
				whole = true;
			else if (v->varattno > 0)
				attnos = bms_add_member(attnos, v->varattno);
		}
	}
	for (i = 1; i <= ncols; i++)
		used = lappend_int(used, whole || bms_is_member(i, attnos));

	/* the scan's tuple: the entry's Vars the targetlist, quals and keys use */
	vars = pull_var_clause((Node *) list_concat(list_concat_copy((List *) tlist, quals),
												keyclauses),
						   PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	attnos = NULL;
	whole = false;
	foreach(lc, vars)
	{
		Var		   *v = lfirst(lc);

		if (!IsA(v, Var) || v->varno != rel->relid)
			continue;
		if (v->varattno == 0)
			whole = true;
		else
			attnos = bms_add_member(attnos, v->varattno);
	}
	i = -1;
	while ((i = bms_next_member(attnos, i)) >= 0)
	{
		Var		   *v = makeVar(rel->relid, i,
								list_nth_oid(rtfunc->funccoltypes, i - 1),
								list_nth_int(rtfunc->funccoltypmods, i - 1),
								list_nth_oid(rtfunc->funccolcollations, i - 1),
								0);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) v,
											 list_length(scan_tlist) + 1,
											 NULL, false));
	}
	if (whole)
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeWholeRowVar(rte, rel->relid, 0, false),
											 list_length(scan_tlist) + 1,
											 NULL, false));

	rte_args(rte, &ref, &schema, &relname, &check_as, &inh);
	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = quals;
	cs->scan.scanrelid = 0;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = keyclauses;
	cs->custom_scan_tlist = scan_tlist;
	cs->custom_relids = bms_copy(rel->relids);
	cs->custom_private = list_make5(list_make5(makeString(ref),
											   makeString(schema),
											   makeString(relname),
											   makeInteger((int) check_as),
											   makeBoolean(inh)),
									list_make4(copyObject(rtfunc->funccolnames),
											   list_copy(rtfunc->funccoltypes),
											   list_copy(rtfunc->funccoltypmods),
											   list_copy(rtfunc->funccolcollations)),
									used,
									list_make2(makeInteger((int) indexid),
											   makeString(indexname ? indexname : "")),
									keyinfo);
	cs->methods = &revision_scan_methods;
	return (Plan *) cs;
}

/* ------------------------------------------------------------------------
 * The executor
 * ------------------------------------------------------------------------
 */

typedef struct RevisionScanState
{
	CustomScanState css;
	RevisionReader rr;
	int			nscan;			/* the scan tuple's columns */
	int		   *scancol;		/* each one's query column (from 0; -1: the
								 * whole row) */
	TupleDesc	rowdesc;		/* the whole row's, if read */
	bool		opened;
} RevisionScanState;

static Node *revision_create_state(CustomScan *cs);
static void revision_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *revision_exec(CustomScanState *node);
static void revision_end(CustomScanState *node);
static void revision_rescan(CustomScanState *node);
static void revision_explain(CustomScanState *node, List *ancestors,
							 ExplainState *es);

static const CustomPathMethods revision_path_methods = {
	.CustomName = "RevisionScan",
	.PlanCustomPath = revision_plan,
};

static const CustomScanMethods revision_scan_methods = {
	.CustomName = "RevisionScan",
	.CreateCustomScanState = revision_create_state,
};

static const CustomExecMethods revision_exec_methods = {
	.CustomName = "RevisionScan",
	.BeginCustomScan = revision_begin,
	.ExecCustomScan = revision_exec,
	.EndCustomScan = revision_end,
	.ReScanCustomScan = revision_rescan,
	.ExplainCustomScan = revision_explain,
};

static Node *
revision_create_state(CustomScan *cs)
{
	RevisionScanState *rs = palloc0_object(RevisionScanState);

	NodeSetTag(rs, T_CustomScanState);
	rs->css.methods = &revision_exec_methods;
	return (Node *) rs;
}

static void
revision_begin(CustomScanState *node, EState *estate, int eflags)
{
	RevisionScanState *rs = (RevisionScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *names = linitial(cs->custom_private);
	List	   *cols = lsecond(cs->custom_private);
	List	   *usedl = lthird(cs->custom_private);
	List	   *index = lfourth(cs->custom_private);
	List	   *keyinfo = list_nth(cs->custom_private, 4);
	RevisionReader *rr = &rs->rr;
	bool	   *used;
	ListCell   *lc;
	int			i;

	rr->ncols = list_length(linitial(cols));
	rr->colnames = palloc_array(char *, rr->ncols);
	rr->coltypes = palloc_array(Oid, rr->ncols);
	used = palloc0_array(bool, rr->ncols);
	i = 0;
	foreach(lc, (List *) linitial(cols))
	{
		rr->colnames[i] = strVal(lfirst(lc));
		rr->coltypes[i] = list_nth_oid(lsecond(cols), i);
		used[i] = list_nth_int(usedl, i) != 0;
		i++;
	}

	/* what the scan tuple holds */
	rs->nscan = list_length(cs->custom_scan_tlist);
	rs->scancol = palloc_array(int, rs->nscan);
	i = 0;
	foreach(lc, cs->custom_scan_tlist)
	{
		Var		   *v = (Var *) ((TargetEntry *) lfirst(lc))->expr;

		rs->scancol[i] = v->varattno - 1;
		if (v->varattno == 0)
		{
			/* (the whole row: each column, a record of them) */
			TupleDesc	d = CreateTemplateTupleDesc(rr->ncols);

			for (int c = 0; c < rr->ncols; c++)
			{
				TupleDescInitEntry(d, c + 1, rr->colnames[c], rr->coltypes[c],
								   list_nth_int(lthird(cols), c), 0);
				TupleDescInitEntryCollation(d, c + 1, list_nth_oid(lfourth(cols), c));
			}
			TupleDescFinalize(d);
			rs->rowdesc = BlessTupleDesc(d);
		}
		i++;
	}
	if (rs->rowdesc != NULL)
		for (int c = 0; c < rr->ncols; c++)
			used[c] = true;

	/* keys, whatever the index: each one's value an expression of the plan */
	rr->nkeys = list_length(keyinfo);
	rr->keys = palloc0_array(RevisionKey, Max(rr->nkeys, 1));
	rr->scankeys = palloc0_array(ScanKeyData, Max(rr->nkeys, 1));
	i = 0;
	foreach(lc, keyinfo)
	{
		List	   *k = lfirst(lc);
		OpExpr	   *clause = list_nth(cs->custom_exprs, i);
		RevisionKey *key = &rr->keys[i];

		key->column = intVal(linitial(k));
		key->strategy = intVal(lsecond(k));
		key->subtype = (Oid) intVal(lthird(k));
		key->collation = (Oid) intVal(lfourth(k));
		key->proc = get_opcode(clause->opno);
		key->value = ExecInitExpr((Expr *) lsecond(clause->args),
								  (PlanState *) node);
		i++;
	}

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	rr->snapshot = estate->es_snapshot;
	rr->check_as = (Oid) intVal(lfourth(names));
	rr->inh = boolVal(list_nth(names, 4));
	reader_open(rr, strVal(linitial(names)), strVal(lsecond(names)),
				strVal(lthird(names)), used,
				rr->nkeys > 0 ? (Oid) intVal(linitial(index)) : InvalidOid);
	rs->opened = true;
}

static TupleTableSlot *
revision_next(ScanState *node)
{
	RevisionScanState *rs = (RevisionScanState *) node;
	RevisionReader *rr = &rs->rr;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	ExprContext *econtext = node->ps.ps_ExprContext;

	ExecClearTuple(slot);
	if (!reader_next(rr, econtext))
		return slot;
	for (int i = 0; i < rs->nscan; i++)
	{
		int			c = rs->scancol[i];

		if (c >= 0)
		{
			slot->tts_values[i] = reader_value(rr, c, &slot->tts_isnull[i]);
			continue;
		}
		else
		{
			Datum	   *values = palloc_array(Datum, rr->ncols);
			bool	   *nulls = palloc_array(bool, rr->ncols);
			MemoryContext old;

			for (int j = 0; j < rr->ncols; j++)
				values[j] = reader_value(rr, j, &nulls[j]);
			old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
			slot->tts_values[i] =
				HeapTupleGetDatum(heap_form_tuple(rs->rowdesc, values, nulls));
			slot->tts_isnull[i] = false;
			MemoryContextSwitchTo(old);
		}
	}
	return ExecStoreVirtualTuple(slot);
}

static bool
revision_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
revision_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, revision_next, revision_recheck);
}

static void
revision_rescan(CustomScanState *node)
{
	RevisionScanState *rs = (RevisionScanState *) node;

	if (rs->opened)
		reader_rescan(&rs->rr);
	ExecScanReScan(&node->ss);
}

static void
revision_end(CustomScanState *node)
{
	RevisionScanState *rs = (RevisionScanState *) node;

	if (rs->opened)
		reader_close(&rs->rr);
	rs->opened = false;
}

static void
revision_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *names = linitial(cs->custom_private);
	List	   *index = lfourth(cs->custom_private);

	ExplainPropertyText("Revision", strVal(linitial(names)), es);
	ExplainPropertyText("Table", quote_qualified_identifier(strVal(lsecond(names)),
															strVal(lthird(names))),
						es);
	if (cs->custom_exprs != NIL)
	{
		List	   *context;
		char	   *cond;

		context = set_deparse_context_plan(es->deparse_cxt,
										   (Plan *) cs, ancestors);
		cond = deparse_expression((Node *) make_ands_explicit(cs->custom_exprs),
								  context, es->verbose, false);
		ExplainPropertyText("Index", strVal(lsecond(index)), es);
		ExplainPropertyText("Index Cond", cond, es);
	}
}

/* ------------------------------------------------------------------------
 * prolly_revision_table(revision text, schema_name text, table_name text)
 *	  returns setof record
 *
 * The rows a revision-qualified name reads, every column of the column
 * definition list read by its name, where the planner doesn't scan the
 * name itself (the function called as such).
 * ------------------------------------------------------------------------
 */
Datum
prolly_revision_table(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	RevisionReader rr;
	TupleDesc	desc;
	bool	   *used;
	Datum	   *values;
	bool	   *nulls;
	int			n = 0;

	/*
	 * Only the parser's call of a revision's name: one written as a call
	 * (refused as it is parsed, but a plan can be made otherwise) could name
	 * the role it is checked as.
	 */
	if (fcinfo->flinfo->fn_expr == NULL ||
		!IsA(fcinfo->flinfo->fn_expr, FuncExpr) ||
		((FuncExpr *) fcinfo->flinfo->fn_expr)->funcformat != COERCE_SQL_SYNTAX)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly_revision_table() can't be called directly"),
				 errhint("Name the table as \"database/revision\".schema.table.")));
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	desc = rsinfo->setDesc;
	memset(&rr, 0, sizeof(rr));
	rr.colnames = palloc_array(char *, desc->natts);
	rr.coltypes = palloc_array(Oid, desc->natts);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		rr.colnames[n] = NameStr(att->attname);
		rr.coltypes[n++] = att->atttypid;
	}
	rr.ncols = n;
	used = palloc_array(bool, Max(n, 1));
	for (int i = 0; i < n; i++)
		used[i] = true;
	rr.snapshot = GetActiveSnapshot();
	rr.check_as = PG_ARGISNULL(3) ? InvalidOid : PG_GETARG_OID(3);
	rr.inh = !PG_ARGISNULL(4) && PG_GETARG_BOOL(4);
	reader_open(&rr, text_to_cstring(PG_GETARG_TEXT_PP(0)),
				text_to_cstring(PG_GETARG_TEXT_PP(1)),
				text_to_cstring(PG_GETARG_TEXT_PP(2)), used, InvalidOid);
	values = palloc_array(Datum, Max(desc->natts, 1));
	nulls = palloc_array(bool, Max(desc->natts, 1));
	while (reader_next(&rr, NULL))
	{
		int			c = 0;

		for (int i = 0; i < desc->natts; i++)
		{
			if (TupleDescAttr(desc, i)->attisdropped)
			{
				nulls[i] = true;
				continue;
			}
			values[i] = reader_value(&rr, c++, &nulls[i]);
		}
		tuplestore_putvalues(rsinfo->setResult, desc, values, nulls);
	}
	reader_close(&rr);
	return (Datum) 0;
}
