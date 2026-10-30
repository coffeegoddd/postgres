/*--------------------------------------------------------------------------
 *
 * bmap_primitive_checks.c
 *		Functions for testing the block map's primitives that later work
 *		builds on: storage made from a held root, the pages changed since
 *		an epoch, and whether a page is shared and with whom.
 *
 * Storage is named by relation file number in the database's default
 * tablespace, as in bmap_checks.c.  Storage made here has no catalog row;
 * test_bmap_unlink() drops it.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/bmap_primitive_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bmap.h"
#include "storage/smgr.h"
#include "utils/tuplestore.h"

static RelFileLocator
locator_of(Oid relnumber)
{
	RelFileLocator loc;

	loc.spcOid = MyDatabaseTableSpace;
	loc.dbOid = MyDatabaseId;
	loc.relNumber = relnumber;
	return loc;
}

static SMgrRelation
bmap_smgr(Oid relnumber)
{
	SMgrRelation srel = smgropen(locator_of(relnumber), INVALID_PROC_NUMBER);

	if (srel->smgr_which != SMGR_BMAP)
		elog(ERROR, "relation file number %u is not block-mapped", relnumber);
	return srel;
}

/*
 * test_bmap_storage_from_root(root, maps_from): a new relation file number
 * whose storage is made of the held root, with copies of maps_from's free
 * space and visibility maps if maps_from isn't 0.
 */
PG_FUNCTION_INFO_V1(test_bmap_storage_from_root);

Datum
test_bmap_storage_from_root(PG_FUNCTION_ARGS)
{
	BmapRootId	root = (BmapRootId) PG_GETARG_INT64(0);
	Oid			maps_from = PG_GETARG_OID(1);
	RelFileNumber copy = GetNewRelFileNumber(MyDatabaseTableSpace, NULL,
											 RELPERSISTENCE_PERMANENT);

	RelationCreateStorageFromRoot(locator_of(copy), root,
								  OidIsValid(maps_from) ? bmap_smgr(maps_from) : NULL,
								  NULL, NULL, NULL, false);
	PG_RETURN_OID(copy);
}

/* test_bmap_raise_epoch(relnumber): the relation's new epoch */
PG_FUNCTION_INFO_V1(test_bmap_raise_epoch);

Datum
test_bmap_raise_epoch(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) bmap_raise_epoch(bmap_smgr(PG_GETARG_OID(0))));
}

/*
 * test_bmap_changed_pages(relnumber, since): the logical blocks whose
 * pages changed in epoch 'since' or after, ascending
 */
PG_FUNCTION_INFO_V1(test_bmap_changed_pages);

Datum
test_bmap_changed_pages(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	BlockNumber *blknos;
	BlockNumber nblocks;
	uint32		n;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	n = bmap_changed_pages(bmap_smgr(PG_GETARG_OID(0)),
						   (uint32) PG_GETARG_INT64(1), &blknos, &nblocks);
	for (uint32 i = 0; i < n; i++)
	{
		Datum		value = Int64GetDatum((int64) blknos[i]);
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &value, &null);
	}
	return (Datum) 0;
}

/*
 * test_bmap_page_shared(relnumber, blkno): whether the page is shared,
 * whether preparing it for a change would copy it, and whether another
 * relation (not only roots held of this one) may hold it
 */
PG_FUNCTION_INFO_V1(test_bmap_page_shared);

Datum
test_bmap_page_shared(PG_FUNCTION_ARGS)
{
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));
	BlockNumber blkno = (BlockNumber) PG_GETARG_INT64(1);
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {0};
	bool		copies;
	bool		shared;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	shared = smgrpageisshared(srel, MAIN_FORKNUM, blkno, &copies);
	values[0] = BoolGetDatum(shared);
	values[1] = BoolGetDatum(copies);
	values[2] = BoolGetDatum(shared &&
							 smgrpagesharedelsewhere(srel, MAIN_FORKNUM, blkno));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
