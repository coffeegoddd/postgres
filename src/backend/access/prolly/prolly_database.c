/*-------------------------------------------------------------------------
 *
 * prolly_database.c
 *	  Keeping prolly state in step with CREATE DATABASE.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_database.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_chunkstore.h"
#include "access/prolly_database.h"
#include "access/prolly_registry.h"

void
ProllyCopyDatabase(Oid src_dboid, Oid src_spcoid, Oid dst_dboid,
				   Oid dst_spcoid)
{
	List	   *impls = prolly_list_impls(PROLLY_IFACE_CHUNK_STORE);
	ListCell   *lc;

	foreach(lc, impls)
	{
		const ProllyChunkStoreRoutine *r = lfirst(lc);

		r->copy_database(src_dboid, src_spcoid, dst_dboid, dst_spcoid);
	}
	list_free(impls);
}
