/*-------------------------------------------------------------------------
 *
 * refs_history.c
 *	  Walks of a commit graph that refs implementations share: each commit
 *	  visited once, however many paths reach it.
 *
 * A history of merges reaches a commit along many paths (each merge
 * doubles them), so a walk that doesn't remember what it visited takes
 * time exponential in the number of merges.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/refs_history.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_refs.h"
#include "miscadmin.h"
#include "utils/hsearch.h"

/* A commit id as a hash table's key: its bytes past its length zeroed. */
static void
id_key(const ProllyHash *id, ProllyHash *key)
{
	memset(key, 0, sizeof(*key));
	key->len = id->len;
	memcpy(key->bytes, id->bytes, id->len);
}

static HTAB *
id_set(const char *name)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Breadth first from start, each commit once: visit(id, arg) for each, in
 * order, until it returns true (then *found gets that id and true is
 * returned).
 */
static bool
walk(const ProllyHash *start, ProllyCommitLookup lookup, void *lookup_arg,
	 bool (*visit) (const ProllyHash *id, void *arg), void *arg,
	 ProllyHash *found)
{
	HTAB	   *seen = id_set("prolly history walk");
	ProllyHash *queue;
	int			head = 0;
	int			tail = 0;
	int			cap = 64;
	bool		hit = false;

	queue = palloc_array(ProllyHash, cap);
	id_key(start, &queue[tail++]);
	(void) hash_search(seen, &queue[0], HASH_ENTER, NULL);
	while (head < tail)
	{
		ProllyHash	id = queue[head++];
		ProllyCommitInfo info;

		CHECK_FOR_INTERRUPTS();
		if (visit(&id, arg))
		{
			*found = id;
			hit = true;
			break;
		}
		if (!lookup(&id, &info, lookup_arg))
			continue;
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash	key;
			bool		known;

			id_key(&info.parents[i], &key);
			(void) hash_search(seen, &key, HASH_ENTER, &known);
			if (known)
				continue;
			if (tail == cap)
			{
				cap *= 2;
				queue = repalloc_array(queue, ProllyHash, cap);
			}
			queue[tail++] = key;
		}
	}
	pfree(queue);
	hash_destroy(seen);
	return hit;
}

static bool
add_ancestor(const ProllyHash *id, void *arg)
{
	(void) hash_search((HTAB *) arg, id, HASH_ENTER, NULL);
	return false;
}

static bool
is_ancestor(const ProllyHash *id, void *arg)
{
	return hash_search((HTAB *) arg, id, HASH_FIND, NULL) != NULL;
}

/*
 * The newest common ancestor of a and b: every ancestor of a (a among
 * them), then breadth first from b to the first of those.  False if they
 * have none.
 */
bool
prolly_history_merge_base(const ProllyHash *a, const ProllyHash *b,
						  ProllyCommitLookup lookup, void *lookup_arg,
						  ProllyHash *base)
{
	HTAB	   *ofa = id_set("prolly merge base");
	ProllyHash	unused;
	bool		found;

	(void) walk(a, lookup, lookup_arg, add_ancestor, ofa, &unused);
	found = walk(b, lookup, lookup_arg, is_ancestor, ofa, base);
	hash_destroy(ofa);
	return found;
}
