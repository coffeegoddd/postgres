/*-------------------------------------------------------------------------
 *
 * vc_refs_pointers.c
 *	  "pointers", references as rows of the history store's pointers
 *	  (pg_history.pointers), in the user's transaction
 *	  (prolly/engine/vc_refs.h).
 *
 * A reference is a row, its name the row's name and its chunk the row's
 * hash; the table's trigger takes, moves and lets go of the roots, in the
 * same transaction, so that a reference and its chunk commit or abort
 * together.  get for update is SELECT ... FOR UPDATE: a second waits for
 * the first's transaction, and then reads the row as that left it.  Each
 * statement reads with a snapshot of its own (not read-only), so it sees
 * what the calls before it in the transaction changed.
 * The statements are planned once a backend.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/engine/vc_refs_pointers.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "prolly/chunk_store.h"
#include "prolly/engine/vc_refs.h"
#include "utils/builtins.h"
#include "varatt.h"

static VcRefs pointers_refs = {.routine = &PointersVcRefs};

static SPIPlanPtr get_plan = NULL;
static SPIPlanPtr lock_plan = NULL;
static SPIPlanPtr set_plan = NULL;
static SPIPlanPtr add_plan = NULL;
static SPIPlanPtr remove_plan = NULL;
static SPIPlanPtr list_plan = NULL;

static SPIPlanPtr
plan(SPIPlanPtr *slot, const char *sql, int nargs, Oid *types)
{
	if (*slot == NULL)
	{
		SPIPlanPtr	p = SPI_prepare(sql, nargs, types);

		if (p == NULL)
			elog(ERROR, "could not plan \"%s\": %s", sql,
				 SPI_result_code_string(SPI_result));
		if (SPI_keepplan(p) != 0)
			elog(ERROR, "could not keep the plan of \"%s\"", sql);
		*slot = p;
	}
	return *slot;
}

static VcRefs *
pointers_open(ChunkStore *cs)
{
	if (cs->routine != &PagesChunkStore)
		elog(ERROR, "the pointers' references are on the history store alone");
	pointers_refs.cs = cs;
	return &pointers_refs;
}

static void
spi_begin(void)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
}

static void
spi_end(void)
{
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

static void
check(int rc, int want, const char *what)
{
	if (rc != want)
		elog(ERROR, "%s of a reference failed: %s", what, SPI_result_code_string(rc));
}

static bytea *
hash_datum(const PHash *h)
{
	bytea	   *b = palloc(VARHDRSZ + PHASH_LEN);

	SET_VARSIZE(b, VARHDRSZ + PHASH_LEN);
	memcpy(VARDATA(b), h->b, PHASH_LEN);
	return b;
}

static void
datum_hash(Datum d, PHash *out)
{
	bytea	   *b = DatumGetByteaPP(d);

	/* (the table's check: twenty bytes) */
	if (VARSIZE_ANY_EXHDR(b) != PHASH_LEN)
		elog(ERROR, "a pointer's hash is %zu bytes", VARSIZE_ANY_EXHDR(b));
	memcpy(out->b, VARDATA_ANY(b), PHASH_LEN);
}

static bool
pointers_get(VcRefs *refs, const char *name, bool for_update, PHash *out)
{
	Oid			types[1] = {TEXTOID};
	Datum		args[1] = {CStringGetTextDatum(name)};
	bool		found;
	int			rc;

	spi_begin();
	rc = SPI_execute_plan(for_update ?
						  plan(&lock_plan, "SELECT hash FROM pg_history.pointers"
							   " WHERE name = $1 FOR UPDATE", 1, types) :
						  plan(&get_plan, "SELECT hash FROM pg_history.pointers"
							   " WHERE name = $1", 1, types),
						  args, NULL, false, 1);
	check(rc, SPI_OK_SELECT, "a read");
	found = SPI_processed == 1;
	if (found)
	{
		bool		isnull;

		datum_hash(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1,
								 &isnull), out);
	}
	spi_end();
	return found;
}

static void
pointers_set(VcRefs *refs, const char *name, const PHash *hash)
{
	Oid			types[2] = {TEXTOID, BYTEAOID};
	Datum		args[2] = {CStringGetTextDatum(name), PointerGetDatum(hash_datum(hash))};
	int			rc;

	spi_begin();
	rc = SPI_execute_plan(plan(&set_plan, "INSERT INTO pg_history.pointers VALUES ($1, $2)"
							   " ON CONFLICT (name) DO UPDATE SET hash = EXCLUDED.hash",
							   2, types),
						  args, NULL, false, 0);
	check(rc, SPI_OK_INSERT, "a change");
	spi_end();
}

/*
 * (ON CONFLICT DO NOTHING: a second adding one name waits for the first's
 * transaction, and adds nothing if it committed)
 */
static bool
pointers_add(VcRefs *refs, const char *name, const PHash *hash)
{
	Oid			types[2] = {TEXTOID, BYTEAOID};
	Datum		args[2] = {CStringGetTextDatum(name), PointerGetDatum(hash_datum(hash))};
	bool		added;
	int			rc;

	spi_begin();
	rc = SPI_execute_plan(plan(&add_plan, "INSERT INTO pg_history.pointers VALUES ($1, $2)"
							   " ON CONFLICT (name) DO NOTHING", 2, types),
						  args, NULL, false, 0);
	check(rc, SPI_OK_INSERT, "an addition");
	added = SPI_processed == 1;
	spi_end();
	return added;
}

static bool
pointers_remove(VcRefs *refs, const char *name)
{
	Oid			types[1] = {TEXTOID};
	Datum		args[1] = {CStringGetTextDatum(name)};
	bool		found;
	int			rc;

	spi_begin();
	rc = SPI_execute_plan(plan(&remove_plan, "DELETE FROM pg_history.pointers"
							   " WHERE name = $1", 1, types),
						  args, NULL, false, 0);
	check(rc, SPI_OK_DELETE, "a removal");
	found = SPI_processed == 1;
	spi_end();
	return found;
}

/*
 * (a prefix's names are those from it up to, not including, the prefix
 * with its last byte one more; the prefix's last byte is not 0xff, as no
 * name's byte of UTF-8 text is)
 */
static void
pointers_list(VcRefs *refs, const char *prefix, VcRefFn fn, void *arg)
{
	Oid			types[2] = {TEXTOID, TEXTOID};
	size_t		plen = strlen(prefix);
	char	   *upper = pstrdup(prefix);
	Datum		args[2];
	char		nulls[2] = {' ', ' '};
	Portal		portal;
	MemoryContext cxt = CurrentMemoryContext;

	if (plen == 0)
		nulls[1] = 'n';
	else
	{
		Assert((unsigned char) upper[plen - 1] < 0xff);
		upper[plen - 1]++;
	}
	args[0] = CStringGetTextDatum(prefix);
	args[1] = plen ? CStringGetTextDatum(upper) : (Datum) 0;
	spi_begin();
	portal = SPI_cursor_open(NULL,
							 plan(&list_plan, "SELECT name, hash FROM pg_history.pointers"
								  " WHERE name >= $1 AND ($2 IS NULL OR name < $2)"
								  " ORDER BY name", 2, types),
							 args, nulls, false);
	for (;;)
	{
		SPI_cursor_fetch(portal, true, 256);
		if (SPI_processed == 0)
			break;
		for (uint64 i = 0; i < SPI_processed; i++)
		{
			HeapTuple	tup = SPI_tuptable->vals[i];
			bool		isnull;
			char	   *name;
			PHash		h;
			MemoryContext old;

			name = TextDatumGetCString(SPI_getbinval(tup, SPI_tuptable->tupdesc, 1, &isnull));
			datum_hash(SPI_getbinval(tup, SPI_tuptable->tupdesc, 2, &isnull), &h);
			/* (fn runs in the caller's context, as it would without SPI) */
			old = MemoryContextSwitchTo(cxt);
			fn(arg, name, &h);
			MemoryContextSwitchTo(old);
		}
		SPI_freetuptable(SPI_tuptable);
	}
	SPI_cursor_close(portal);
	spi_end();
	pfree(upper);
}

const VcRefsRoutine PointersVcRefs = {
	.open = pointers_open,
	.get = pointers_get,
	.set = pointers_set,
	.add = pointers_add,
	.remove = pointers_remove,
	.list = pointers_list,
};
