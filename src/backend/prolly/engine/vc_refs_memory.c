/*-------------------------------------------------------------------------
 *
 * vc_refs_memory.c
 *	  "memory", a backend's references in memory, on its memory chunk
 *	  store, for unit tests (prolly/engine/vc_refs.h).
 *
 * The references are the backend's alone, kept in a hash table that lasts
 * as long as the backend (or MemoryVcRefsReset).  As the memory store,
 * they change at once, not by transactions, and take and let go of their
 * chunks' roots themselves.  Locking does nothing: a backend's references
 * are its own.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/engine/vc_refs_memory.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/engine/vc_refs.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#define REF_NAME_MAX	256

typedef struct MemRef
{
	char		name[REF_NAME_MAX];	/* the key */
	PHash		hash;
} MemRef;

static HTAB *refs_table = NULL;
static VcRefs mem_refs = {.routine = &MemoryVcRefs};

static void
make_table(void)
{
	HASHCTL		ctl;

	ctl.keysize = REF_NAME_MAX;
	ctl.entrysize = sizeof(MemRef);
	refs_table = hash_create("memory vc refs", 256, &ctl, HASH_ELEM | HASH_STRINGS);
}

static VcRefs *
mem_refs_open(ChunkStore *cs)
{
	if (cs->routine != &MemoryChunkStore)
		elog(ERROR, "the memory references are on the memory chunk store alone");
	if (refs_table == NULL)
		make_table();
	mem_refs.cs = cs;
	return &mem_refs;
}

static void
check_name(const char *name)
{
	if (strlen(name) >= REF_NAME_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("reference name \"%s\" is too long", name)));
}

static bool
mem_refs_get(VcRefs *refs, const char *name, bool for_update, PHash *out)
{
	MemRef	   *r;

	check_name(name);
	r = hash_search(refs_table, name, HASH_FIND, NULL);
	if (r == NULL)
		return false;
	*out = r->hash;
	return true;
}

static void
mem_refs_set(VcRefs *refs, const char *name, const PHash *hash)
{
	MemRef	   *r;
	bool		found;

	check_name(name);
	/* (the new root first: an error leaves the old reference as it was) */
	cs_ref_root(refs->cs, hash->b);
	r = hash_search(refs_table, name, HASH_ENTER, &found);
	if (found)
		cs_unref_root(refs->cs, r->hash.b);
	r->hash = *hash;
}

static bool
mem_refs_add(VcRefs *refs, const char *name, const PHash *hash)
{
	MemRef	   *r;
	bool		found;

	check_name(name);
	if (hash_search(refs_table, name, HASH_FIND, NULL) != NULL)
		return false;
	cs_ref_root(refs->cs, hash->b);
	r = hash_search(refs_table, name, HASH_ENTER, &found);
	r->hash = *hash;
	return true;
}

static bool
mem_refs_remove(VcRefs *refs, const char *name)
{
	MemRef	   *r;

	check_name(name);
	r = hash_search(refs_table, name, HASH_FIND, NULL);
	if (r == NULL)
		return false;
	cs_unref_root(refs->cs, r->hash.b);
	(void) hash_search(refs_table, name, HASH_REMOVE, NULL);
	return true;
}

static int
ref_cmp(const void *a, const void *b)
{
	return strcmp(((const MemRef *) a)->name, ((const MemRef *) b)->name);
}

static void
mem_refs_list(VcRefs *refs, const char *prefix, VcRefFn fn, void *arg)
{
	HASH_SEQ_STATUS seq;
	MemRef	   *r;
	MemRef	   *found = palloc_array(MemRef, Max(hash_get_num_entries(refs_table), 1));
	int			n = 0;
	size_t		plen = strlen(prefix);

	/* (copied first: fn may change the references) */
	hash_seq_init(&seq, refs_table);
	while ((r = hash_seq_search(&seq)) != NULL)
		if (strncmp(r->name, prefix, plen) == 0)
			found[n++] = *r;
	qsort(found, n, sizeof(MemRef), ref_cmp);
	for (int i = 0; i < n; i++)
		fn(arg, found[i].name, &found[i].hash);
	pfree(found);
}

void
MemoryVcRefsReset(void)
{
	if (refs_table == NULL)
		return;
	hash_destroy(refs_table);
	make_table();
}

const VcRefsRoutine MemoryVcRefs = {
	.open = mem_refs_open,
	.get = mem_refs_get,
	.set = mem_refs_set,
	.add = mem_refs_add,
	.remove = mem_refs_remove,
	.list = mem_refs_list,
};
