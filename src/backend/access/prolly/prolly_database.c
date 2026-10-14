/*-------------------------------------------------------------------------
 *
 * prolly_database.c
 *	  Keeping prolly state in step with CREATE DATABASE and DROP DATABASE.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_database.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_commit.h"
#include "access/prolly_database.h"
#include "access/prolly_registry.h"

void
ProllyCopyDatabase(Oid src_dboid, Oid dst_dboid)
{
	List	   *impls = prolly_list_impls(PROLLY_IFACE_COMMIT_PIPELINE);
	ListCell   *lc;

	foreach(lc, impls)
	{
		const ProllyCommitRoutine *r = lfirst(lc);

		if (PROLLY_HAS(r, copy_database))
			r->copy_database(src_dboid, dst_dboid);
	}
	list_free(impls);
}

void
ProllyDropDatabase(Oid dboid)
{
	List	   *impls = prolly_list_impls(PROLLY_IFACE_COMMIT_PIPELINE);
	ListCell   *lc;

	foreach(lc, impls)
	{
		const ProllyCommitRoutine *r = lfirst(lc);

		if (PROLLY_HAS(r, drop_database))
			r->drop_database(dboid);
	}
	list_free(impls);
}
