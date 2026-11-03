/*-------------------------------------------------------------------------
 *
 * vc_refs_memory.c
 *	  "memory", a backend's references in memory, on its memory chunk
 *	  store, for unit tests (prolly/engine/vc_refs.h).
 *
 * The references are the backend's alone, a set for each memory store,
 * kept in a hash table that lasts as long as the backend (or until
 * MemoryVcRefsReset, or MemoryVcRefsForget for its store): a test's own
 * store carries a repository of its own, a remote's.  As the memory store,
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

/* One store's references (base first: a VcRefs * is one of these). */
typedef struct MemRefs
{
	VcRefs		base;
	HTAB	   *table;
	struct MemRefs *next;
} MemRefs;

static MemRefs *all_refs = NULL;

#define TABLE(refs) (((MemRefs *) (refs))->table)

static HTAB *
make_table(void)
{
	HASHCTL		ctl;

	ctl.keysize = REF_NAME_MAX;
	ctl.entrysize = sizeof(MemRef);
	ctl.hcxt = TopMemoryContext;
	return hash_create("memory vc refs", 256, &ctl,
					   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
}

static VcRefs *
mem_refs_open(ChunkStore *cs)
{
	MemRefs    *m;

	if (cs->routine != &MemoryChunkStore)
		elog(ERROR, "the memory references are on the memory chunk store alone");
	for (m = all_refs; m != NULL; m = m->next)
		if (m->base.cs == cs)
			return &m->base;
	m = MemoryContextAllocZero(TopMemoryContext, sizeof(MemRefs));
	m->base.routine = &MemoryVcRefs;
	m->base.cs = cs;
	m->table = make_table();
	m->next = all_refs;
	all_refs = m;
	return &m->base;
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
mem_refs_get(VcRefs *refs, const char *name, VcRefLock lock, PHash *out)
{
	MemRef	   *r;

	/* (a name too long to make is not there, as with the pointers) */
	if (strlen(name) >= REF_NAME_MAX)
		return false;
	r = hash_search(TABLE(refs), name, HASH_FIND, NULL);
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
	r = hash_search(TABLE(refs), name, HASH_ENTER, &found);
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
	if (hash_search(TABLE(refs), name, HASH_FIND, NULL) != NULL)
		return false;
	cs_ref_root(refs->cs, hash->b);
	r = hash_search(TABLE(refs), name, HASH_ENTER, &found);
	r->hash = *hash;
	return true;
}

static bool
mem_refs_remove(VcRefs *refs, const char *name)
{
	MemRef	   *r;

	if (strlen(name) >= REF_NAME_MAX)
		return false;
	r = hash_search(TABLE(refs), name, HASH_FIND, NULL);
	if (r == NULL)
		return false;
	cs_unref_root(refs->cs, r->hash.b);
	(void) hash_search(TABLE(refs), name, HASH_REMOVE, NULL);
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
	MemRef	   *found = palloc_array(MemRef, Max(hash_get_num_entries(TABLE(refs)), 1));
	int			n = 0;
	size_t		plen = strlen(prefix);

	/* (copied first: fn may change the references) */
	hash_seq_init(&seq, TABLE(refs));
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
	while (all_refs != NULL)
	{
		MemRefs    *m = all_refs;

		all_refs = m->next;
		hash_destroy(m->table);
		pfree(m);
	}
}

void
MemoryVcRefsForget(ChunkStore *cs)
{
	MemRefs   **link;

	for (link = &all_refs; *link != NULL; link = &(*link)->next)
		if ((*link)->base.cs == cs)
		{
			MemRefs    *m = *link;

			*link = m->next;
			hash_destroy(m->table);
			pfree(m);
			return;
		}
}

const VcRefsRoutine MemoryVcRefs = {
	.open = mem_refs_open,
	.get = mem_refs_get,
	.set = mem_refs_set,
	.add = mem_refs_add,
	.remove = mem_refs_remove,
	.list = mem_refs_list,
};
