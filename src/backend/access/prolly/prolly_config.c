/*-------------------------------------------------------------------------
 *
 * prolly_config.c
 *	  Choosing which implementation of each interface a tree uses.
 *
 * Each prolly table AM has a default profile: an implementation name per
 * interface.  The pairings follow specs/04-candidate-designs.md.  Foundation
 * interfaces have only stubs so far.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
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

#define FOUNDATION_STUBS \
	[PROLLY_IFACE_NODE_FORMAT] = "stub", \
	[PROLLY_IFACE_CHUNKER] = "stub", \
	[PROLLY_IFACE_CONTENT_HASH] = "stub", \
	[PROLLY_IFACE_KEY_ENCODER] = "stub", \
	[PROLLY_IFACE_VALUE_ENCODER] = "stub", \
	[PROLLY_IFACE_NODE_STORE] = "stub", \
	[PROLLY_IFACE_ALLOCATOR] = "stub", \
	[PROLLY_IFACE_RECLAMATION] = "stub", \
	[PROLLY_IFACE_LOG] = "stub", \
	[PROLLY_IFACE_COMMIT_PIPELINE] = "stub", \
	[PROLLY_IFACE_ROW_IDENTITY] = "stub"

static const ProllyProfile profiles[] = {
	{"prolly_d1", {[PROLLY_IFACE_ENGINE] = "d1",
	[PROLLY_IFACE_COORDINATION] = "k1", FOUNDATION_STUBS}},
	{"prolly_d2", {[PROLLY_IFACE_ENGINE] = "d2",
	[PROLLY_IFACE_COORDINATION] = "k1", FOUNDATION_STUBS}},
	{"prolly_d3a", {[PROLLY_IFACE_ENGINE] = "d3a",
	[PROLLY_IFACE_COORDINATION] = "k2", FOUNDATION_STUBS}},
	{"prolly_d3b", {[PROLLY_IFACE_ENGINE] = "d3b",
	[PROLLY_IFACE_COORDINATION] = "k2", FOUNDATION_STUBS}},
	{"prolly_d4", {[PROLLY_IFACE_ENGINE] = "d4",
	[PROLLY_IFACE_COORDINATION] = "k3", FOUNDATION_STUBS}},
};

static const char *const database_profile[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_REFS] = "stub",
	[PROLLY_IFACE_TRANSFER] = "stub",
	[PROLLY_IFACE_DIFF_MERGE] = "stub",
};

static const ProllyImplInfo *
profile_impl(const char *const *impls, ProllyInterface iface)
{
	Assert(impls[iface] != NULL);
	return prolly_get_impl(iface, impls[iface]);
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
}

void
prolly_components_for_relation(Relation rel, ProllyComponents *components)
{
	Oid			amoid;

	/* An index's tree belongs to its table's engine (invariant 6). */
	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		amoid = get_rel_relam(rel->rd_index->indrelid);
	else
		amoid = rel->rd_rel->relam;

	prolly_components_for_am(get_am_name(amoid), components);
}

void
prolly_database_components(ProllyDatabaseComponents *components)
{
	components->refs = (const ProllyRefsRoutine *)
		profile_impl(database_profile, PROLLY_IFACE_REFS);
	components->transfer = (const ProllyTransferRoutine *)
		profile_impl(database_profile, PROLLY_IFACE_TRANSFER);
	components->diff_merge = (const ProllyMergeRoutine *)
		profile_impl(database_profile, PROLLY_IFACE_DIFF_MERGE);
}
