/*-------------------------------------------------------------------------
 *
 * prolly_config.c
 *	  Choosing which implementation of each interface a tree uses.
 *
 * Each prolly table AM has a default profile: an implementation name per
 * interface, pairing each engine with a coordination module.  The
 * foundation implementations are shared by every engine; interfaces without
 * one yet use their stubs.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"

#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "catalog/branch_ops.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

typedef struct ProllyProfile
{
	const char *amname;
	/* implementation name per tree-level interface, by ProllyInterface */
	const char *impls[PROLLY_IFACE_LAST + 1];
} ProllyProfile;

/*
 * Shared by every engine.  Node store, tree, root layout, log, commit
 * pipeline and row identity differ by engine: D1 and D2 are hash-addressed
 * (the "global" store, copy-on-write trees, sub-root layout, serial
 * commits, hashed TIDs); D3 and D4 are location-addressed and have none of
 * these yet.
 */
#define FOUNDATION \
	[PROLLY_IFACE_NODE_FORMAT] = "basic", \
	[PROLLY_IFACE_CHUNKER] = "keyhash", \
	[PROLLY_IFACE_CONTENT_HASH] = "sha256", \
	[PROLLY_IFACE_KEY_ENCODER] = "ordered", \
	[PROLLY_IFACE_VALUE_ENCODER] = "canonical", \
	[PROLLY_IFACE_TABLE_KEYING] = "pk_or_content", \
	[PROLLY_IFACE_INDEX_KEYING] = "columns_then_row", \
	[PROLLY_IFACE_ALLOCATOR] = "stub"

#define HASH_ADDRESSED \
	[PROLLY_IFACE_NODE_STORE] = "global", \
	[PROLLY_IFACE_TREE] = "cow", \
	[PROLLY_IFACE_ROOT_LAYOUT] = "subroots", \
	[PROLLY_IFACE_COMMIT_PIPELINE] = "serial", \
	[PROLLY_IFACE_ROW_IDENTITY] = "counter", \
	[PROLLY_IFACE_RECLAMATION] = "mark_sweep"

#define NOT_YET \
	[PROLLY_IFACE_NODE_STORE] = "stub", \
	[PROLLY_IFACE_TREE] = "stub", \
	[PROLLY_IFACE_ROOT_LAYOUT] = "stub", \
	[PROLLY_IFACE_COMMIT_PIPELINE] = "stub", \
	[PROLLY_IFACE_ROW_IDENTITY] = "stub", \
	[PROLLY_IFACE_RECLAMATION] = "stub", \
	[PROLLY_IFACE_LOG] = "stub"

static const ProllyProfile profiles[] = {
	{"prolly_d1", {[PROLLY_IFACE_ENGINE] = "d1",
	[PROLLY_IFACE_COORDINATION] = "k1", HASH_ADDRESSED,
	[PROLLY_IFACE_LOG] = "chunks", FOUNDATION}},
	{"prolly_d2", {[PROLLY_IFACE_ENGINE] = "d2",
	[PROLLY_IFACE_COORDINATION] = "k1", HASH_ADDRESSED,
	[PROLLY_IFACE_LOG] = "stub", FOUNDATION}},
	{"prolly_d3a", {[PROLLY_IFACE_ENGINE] = "d3a",
	[PROLLY_IFACE_COORDINATION] = "k2", NOT_YET, FOUNDATION}},
	{"prolly_d3b", {[PROLLY_IFACE_ENGINE] = "d3b",
	[PROLLY_IFACE_COORDINATION] = "k2", NOT_YET, FOUNDATION}},
	{"prolly_d4", {[PROLLY_IFACE_ENGINE] = "d4",
	[PROLLY_IFACE_COORDINATION] = "k3", NOT_YET, FOUNDATION}},
};

/*
 * Block-mapped prolly tables keep their rows in pages, and have no engine:
 * these are the components their rows' history is folded with (keying,
 * encoders, hash, and trees of chunks).  Not in profiles[]: the access
 * method isn't one of the engines'.
 */
static const ProllyProfile history_profile =
{"prolly", {
		[PROLLY_IFACE_CHUNKER] = "bytes",
		[PROLLY_IFACE_CONTENT_HASH] = "sha256",
		[PROLLY_IFACE_KEY_ENCODER] = "ordered",
		[PROLLY_IFACE_VALUE_ENCODER] = "canonical",
		[PROLLY_IFACE_TABLE_KEYING] = "pk_or_prefix",
		[PROLLY_IFACE_INDEX_KEYING] = "columns_then_row",
		[PROLLY_IFACE_TREE] = "cow",
}};

static const char *const database_profile[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_REFS] = "tree",
	[PROLLY_IFACE_TRANSFER] = "stub",
	[PROLLY_IFACE_DIFF_MERGE] = "rows",
	[PROLLY_IFACE_CANONICALIZER] = "catalogs",
};

/* NULL for an interface the profile has no implementation of */
/*
 * A database whose branches are copies of its versioned schemas: commits
 * of them, folded, in its chunk store.
 */
static const char *const copies_profile[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_REFS] = "copies",
	[PROLLY_IFACE_TRANSFER] = "chunks",
	[PROLLY_IFACE_DIFF_MERGE] = "folded",
	[PROLLY_IFACE_CANONICALIZER] = "folded",
};

static const ProllyImplInfo *
profile_impl(const char *const *impls, ProllyInterface iface)
{
	if (impls[iface] == NULL)
		return NULL;
	return prolly_get_impl(iface, impls[iface]);
}

bool
prolly_is_table_am(const char *amname)
{
	for (int i = 0; amname != NULL && i < lengthof(profiles); i++)
	{
		if (strcmp(profiles[i].amname, amname) == 0)
			return true;
	}
	return false;
}

Oid
prolly_table_am_for_persistence(Oid amoid, char relpersistence, bool explicit)
{
	char	   *amname;
	bool		prolly;

	if (relpersistence == RELPERSISTENCE_PERMANENT || !OidIsValid(amoid))
	{
		prolly_check_versioned_am(amoid, relpersistence);
		return amoid;
	}
	amname = get_am_name(amoid);
	prolly = prolly_is_table_am(amname) || amoid == PROLLY_TABLE_AM_OID;
	if (amname)
		pfree(amname);
	if (!prolly)
		return amoid;
	if (explicit)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly storage supports only permanent tables"),
				 errdetail("Temporary and unlogged tables are never part of a commit."),
				 errhint("Leave out USING, and the table is stored by heap.")));
	return HEAP_TABLE_AM_OID;
}

void
prolly_check_unlogged(Relation rel)
{
	char	   *amname = get_am_name(rel->rd_rel->relam);
	bool		prolly = prolly_is_table_am(amname) ||
		rel->rd_rel->relam == PROLLY_TABLE_AM_OID;

	if (amname)
		pfree(amname);
	if (prolly)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot change prolly table \"%s\" to unlogged",
						RelationGetRelationName(rel)),
				 errdetail("Prolly tables are always permanent."),
				 errhint("Use ALTER TABLE ... SET ACCESS METHOD heap first.")));
}

void
prolly_check_versioned_am(Oid amoid, char relpersistence)
{
	char	   *amname;

	if (!MyDatabaseVersioned || relpersistence != RELPERSISTENCE_PERMANENT ||
		!OidIsValid(amoid))
		return;
	amname = get_am_name(amoid);
	if (!prolly_is_table_am(amname))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a versioned database can't have permanent tables stored by \"%s\"",
						amname ? amname : "?"),
				 errdetail("Every permanent table of a versioned database is versioned, so it is a prolly table."),
				 errhint("Use a prolly table access method, or a temporary or unlogged table.")));
	pfree(amname);
}

void
prolly_note_tablespace(Oid amoid, const char *tablespacename)
{
	char	   *amname;

	if (tablespacename == NULL || !OidIsValid(amoid))
		return;
	amname = get_am_name(amoid);
	if (amname && (prolly_is_table_am(amname) || strcmp(amname, "prolly_d1_index") == 0))
		ereport(NOTICE,
				(errmsg("prolly relations ignore tablespaces; tablespace \"%s\" is not used",
						tablespacename),
				 errdetail("Their rows are kept in the cluster's node store.")));
	if (amname)
		pfree(amname);
}


/*
 * The tree engine of components (those of access method amname, or of
 * table relname's): an error if there is none, as for page-based prolly,
 * whose rows aren't in trees.
 */
const ProllyEngineRoutine *
prolly_engine_of(const ProllyComponents *components, const char *amname,
				 const char *relname)
{
	if (components->engine == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("access method \"%s\" has no prolly tree engine", amname),
				 relname ? errdetail("Table \"%s\" is of it.", relname) : 0));
	return components->engine;
}

void
prolly_components_for_am(const char *amname, ProllyComponents *components)
{
	const ProllyProfile *profile = NULL;
	const char *const *impls;

	for (int i = 0; amname != NULL && i < lengthof(profiles); i++)
	{
		if (strcmp(profiles[i].amname, amname) == 0)
		{
			profile = &profiles[i];
			break;
		}
	}
	if (profile == NULL && amname != NULL &&
		strcmp(amname, history_profile.amname) == 0)
		profile = &history_profile;
	if (profile == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("access method \"%s\" is not a prolly table access method",
						amname ? amname : "(none)")));

	impls = profile->impls;
	components->engine = (const ProllyEngineRoutine *)
		profile_impl(impls, PROLLY_IFACE_ENGINE);
	components->coord = (const ProllyCoordRoutine *)
		profile_impl(impls, PROLLY_IFACE_COORDINATION);
	components->node_format = (const ProllyNodeFormatRoutine *)
		profile_impl(impls, PROLLY_IFACE_NODE_FORMAT);
	components->chunker = (const ProllyChunkerRoutine *)
		profile_impl(impls, PROLLY_IFACE_CHUNKER);
	components->hash = (const ProllyHashRoutine *)
		profile_impl(impls, PROLLY_IFACE_CONTENT_HASH);
	components->key_encoder = (const ProllyKeyEncoderRoutine *)
		profile_impl(impls, PROLLY_IFACE_KEY_ENCODER);
	components->value_encoder = (const ProllyValueEncoderRoutine *)
		profile_impl(impls, PROLLY_IFACE_VALUE_ENCODER);
	components->node_store = (const ProllyNodeStoreRoutine *)
		profile_impl(impls, PROLLY_IFACE_NODE_STORE);
	components->allocator = (const ProllyAllocatorRoutine *)
		profile_impl(impls, PROLLY_IFACE_ALLOCATOR);
	components->reclamation = (const ProllyReclaimRoutine *)
		profile_impl(impls, PROLLY_IFACE_RECLAMATION);
	components->log = (const ProllyLogRoutine *)
		profile_impl(impls, PROLLY_IFACE_LOG);
	components->commit = (const ProllyCommitRoutine *)
		profile_impl(impls, PROLLY_IFACE_COMMIT_PIPELINE);
	components->row_identity = (const ProllyRowIdRoutine *)
		profile_impl(impls, PROLLY_IFACE_ROW_IDENTITY);
	components->tree = (const ProllyTreeRoutine *)
		profile_impl(impls, PROLLY_IFACE_TREE);
	components->root_layout = (const ProllyRootLayoutRoutine *)
		profile_impl(impls, PROLLY_IFACE_ROOT_LAYOUT);
	components->keying = (const ProllyKeyingRoutine *)
		profile_impl(impls, PROLLY_IFACE_TABLE_KEYING);
	components->index_keying = (const ProllyIndexKeyingRoutine *)
		profile_impl(impls, PROLLY_IFACE_INDEX_KEYING);
}

void
prolly_components_for_relation(Relation rel, ProllyComponents *components)
{
	Oid			amoid;

	/*
	 * An index's tree belongs to its table's engine, so a table and its
	 * indexes always come from the same root set.
	 */
	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		amoid = get_rel_relam(rel->rd_index->indrelid);
	else
		amoid = rel->rd_rel->relam;

	prolly_components_for_am(get_am_name(amoid), components);
}

void
prolly_components_for_engine(const ProllyImplInfo *engine,
							 ProllyComponents *components)
{
	for (int i = 0; i < lengthof(profiles); i++)
	{
		const char *name = profiles[i].impls[PROLLY_IFACE_ENGINE];

		if (name != NULL && strcmp(name, engine->name) == 0)
		{
			prolly_components_for_am(profiles[i].amname, components);
			return;
		}
	}
	elog(ERROR, "no prolly profile uses engine \"%s\"", engine->name);
}

/*
 * Are this database's branches copies of its versioned schemas?  Once the
 * copies' refs hold its history (the chunk store has a root), they stay
 * its refs, whatever is dropped later: the last versioned schema dropped is
 * a change to commit, not a return to a database without history.
 */
bool
prolly_database_uses_copies(void)
{
	ProllyHash	root;

	if (database_uses_branch_copies())
		return true;
	if (MyDatabaseVersioned)
		return false;
	PROLLY_CALL(prolly_chunk_store(), root,
				PROLLY_CALL(prolly_chunk_store(), open, MyDatabaseId,
							MyDatabaseTableSpace),
				&root);
	return root.len > 0;
}

static void
components_of(const char *const *impls, ProllyDatabaseComponents *components)
{
	components->refs = (const ProllyRefsRoutine *)
		profile_impl(impls, PROLLY_IFACE_REFS);
	components->transfer = (const ProllyTransferRoutine *)
		profile_impl(impls, PROLLY_IFACE_TRANSFER);
	components->diff_merge = (const ProllyMergeRoutine *)
		profile_impl(impls, PROLLY_IFACE_DIFF_MERGE);
	components->canonicalizer = (const ProllyCanonRoutine *)
		profile_impl(impls, PROLLY_IFACE_CANONICALIZER);
}

void
prolly_database_components(ProllyDatabaseComponents *components)
{
	components_of(prolly_database_uses_copies() ? copies_profile : database_profile,
				  components);
}

/*
 * The components of a database whose branches are copies of its versioned
 * schemas, whether it has any yet or not: for what makes its first commits
 * (a fetch or clone into an empty database).  Not for a versioned database,
 * whose history is the tree engine's.
 */
void
prolly_copies_components(ProllyDatabaseComponents *components)
{
	if (MyDatabaseVersioned)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("remotes need a database of versioned schemas, not a versioned database")));
	components_of(copies_profile, components);
}
