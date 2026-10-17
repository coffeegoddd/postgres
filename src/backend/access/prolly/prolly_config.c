/*-------------------------------------------------------------------------
 *
 * prolly_config.c
 *	  Choosing which implementation of each interface prolly uses.
 *
 * A prolly table's rows are in pages; the components its rows' history is
 * folded with (keying, encoders, hash, and trees of chunks) are its
 * access method's profile.  A database's version control (refs, transfer,
 * diff and merge, canonical form) is its branches' as copies of its
 * versioned schemas.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* The prolly table access method's components, by ProllyInterface. */
static const char *const history_profile[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_CHUNKER] = "bytes",
	[PROLLY_IFACE_CONTENT_HASH] = "sha256",
	[PROLLY_IFACE_KEY_ENCODER] = "ordered",
	[PROLLY_IFACE_VALUE_ENCODER] = "canonical",
	[PROLLY_IFACE_TABLE_KEYING] = "pk_or_prefix",
	[PROLLY_IFACE_TREE] = "cow",
};

/*
 * A database's version control: its branches are copies of its versioned
 * schemas, their commits folded into its chunk store.
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

Oid
prolly_table_am_for_persistence(Oid amoid, char relpersistence, bool explicit)
{
	if (relpersistence == RELPERSISTENCE_PERMANENT || amoid != PROLLY_TABLE_AM_OID)
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
	if (rel->rd_rel->relam == PROLLY_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot change prolly table \"%s\" to unlogged",
						RelationGetRelationName(rel)),
				 errdetail("Prolly tables are always permanent."),
				 errhint("Use ALTER TABLE ... SET ACCESS METHOD heap first.")));
}

void
prolly_components_for_relation(Relation rel, ProllyComponents *components)
{
	Oid			amoid;

	/* (an index's are its table's) */
	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		amoid = get_rel_relam(rel->rd_index->indrelid);
	else
		amoid = rel->rd_rel->relam;

	if (amoid != PROLLY_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a prolly table",
						RelationGetRelationName(rel))));
	components->chunker = (const ProllyChunkerRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_CHUNKER);
	components->hash = (const ProllyHashRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_CONTENT_HASH);
	components->key_encoder = (const ProllyKeyEncoderRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_KEY_ENCODER);
	components->value_encoder = (const ProllyValueEncoderRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_VALUE_ENCODER);
	components->tree = (const ProllyTreeRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_TREE);
	components->keying = (const ProllyKeyingRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_TABLE_KEYING);
}

void
prolly_database_components(ProllyDatabaseComponents *components)
{
	components->refs = (const ProllyRefsRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_REFS);
	components->transfer = (const ProllyTransferRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_TRANSFER);
	components->diff_merge = (const ProllyMergeRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_DIFF_MERGE);
	components->canonicalizer = (const ProllyCanonRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_CANONICALIZER);
}
