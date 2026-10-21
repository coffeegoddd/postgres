/*-------------------------------------------------------------------------
 *
 * refs_history.c
 *	  Walks of a commit graph that refs implementations share: each commit
 *	  visited once per walk, however many paths reach it.
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
#include "nodes/pg_list.h"
#include "utils/hsearch.h"

static HTAB *
id_set(const char *name)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/* What a walk does at a commit. */
typedef enum WalkStep
{
	WALK_ON,					/* on to its parents */
	WALK_PAST,					/* not on to its parents */
	WALK_STOP,					/* the walk ends here */
} WalkStep;

/*
 * Breadth first from the commits of starts, each commit once: visit(id,
 * arg) for each, in order, until it says to stop (then *found gets that
 * id and true is returned).
 */
static bool
walk_from(const ProllyHash *starts, int nstarts, ProllyCommitLookup lookup,
		  void *lookup_arg, WalkStep (*visit) (const ProllyHash *id, void *arg),
		  void *arg, ProllyHash *found)
{
	HTAB	   *seen = id_set("prolly history walk");
	ProllyHash *queue;
	int			head = 0;
	int			tail = 0;
	int			cap = Max(64, nstarts);
	bool		hit = false;

	queue = palloc_array(ProllyHash, cap);
	for (int i = 0; i < nstarts; i++)
	{
		bool		known;

		queue[tail] = prolly_hash_key(&starts[i]);
		(void) hash_search(seen, &queue[tail], HASH_ENTER, &known);
		if (!known)
			tail++;
	}
	while (head < tail)
	{
		ProllyHash	id = queue[head++];
		ProllyCommitInfo info;
		WalkStep	step;

		CHECK_FOR_INTERRUPTS();
		step = visit(&id, arg);
		if (step == WALK_STOP)
		{
			*found = id;
			hit = true;
			break;
		}
		if (step == WALK_PAST || !lookup(&id, &info, lookup_arg))
			continue;
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash	key;
			bool		known;

			key = prolly_hash_key(&info.parents[i]);
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

static WalkStep
add_ancestor(const ProllyHash *id, void *arg)
{
	(void) hash_search((HTAB *) arg, id, HASH_ENTER, NULL);
	return WALK_ON;
}

/* first_common()'s walk: a's ancestors, and the common ones met. */
typedef struct FirstCommon
{
	HTAB	   *ofa;
	ProllyHash *found;
	int			nfound;
	int			cap;
} FirstCommon;

static WalkStep
note_common(const ProllyHash *id, void *arg)
{
	FirstCommon *fc = arg;

	if (hash_search(fc->ofa, id, HASH_FIND, NULL) == NULL)
		return WALK_ON;
	if (fc->nfound == fc->cap)
	{
		fc->cap *= 2;
		fc->found = repalloc_array(fc->found, ProllyHash, fc->cap);
	}
	fc->found[fc->nfound++] = *id;
	return WALK_PAST;
}

/*
 * Breadth first from b, the common ancestors of it and a met first on each
 * path (ofa: a's ancestors), into *found: every maximal one is among
 * them, since a walk past one reaches only its own ancestors.
 */
static int
first_common(const ProllyHash *b, HTAB *ofa, ProllyCommitLookup lookup,
			 void *lookup_arg, ProllyHash **found)
{
	FirstCommon fc = {ofa, palloc_array(ProllyHash, 4), 0, 4};
	ProllyHash	unused;

	(void) walk_from(b, 1, lookup, lookup_arg, note_common, &fc, &unused);
	*found = fc.found;
	return fc.nfound;
}

/*
 * A commit's height, the most commits on a path from it to a root of its
 * history (a root's is 1), as Dolt keeps one in each commit: heights,
 * keyed by id, holds those known.  Found depth first, without recursion,
 * each commit read once: a commit's frame stays on the stack, under its
 * parents', until theirs are found, and each frame records the one that
 * pushed it, whose height it raises as its own is found.
 */
typedef struct Height
{
	ProllyHash	id;				/* (the key) */
	int64		height;
} Height;

typedef struct HeightFrame
{
	ProllyHash	id;
	int			pusher;			/* the frame that pushed it; -1: none */
	bool		read;			/* looked up, its parents pushed */
	int64		height;			/* the most found so far */
} HeightFrame;

HTAB *
prolly_history_heights(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(Height);
	ctl.hcxt = CurrentMemoryContext;
	return hash_create("prolly commit heights", 256, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

int64
prolly_history_height(const ProllyHash *id, HTAB *heights,
					  ProllyCommitLookup lookup, void *lookup_arg)
{
	HeightFrame *stack;
	int			depth = 0;
	int			cap = 64;
	int64		result = 0;
	Height	   *h;

	stack = palloc_array(HeightFrame, cap);
	stack[depth++] = (HeightFrame) {prolly_hash_key(id), -1, false, 1};
	while (depth > 0)
	{
		int			top = depth - 1;
		bool		known = false;

		CHECK_FOR_INTERRUPTS();
		if (!stack[top].read)
		{
			ProllyCommitInfo info;

			stack[top].read = true;
			h = hash_search(heights, &stack[top].id, HASH_FIND, NULL);
			if (h != NULL)
			{
				stack[top].height = h->height;
				known = true;
			}
			else if (!lookup(&stack[top].id, &info, lookup_arg))
			{
				/* (one not found is taken for a root) */
			}
			else if (info.has_height && info.height > 0)
				stack[top].height = info.height;
			else
			{
				for (int i = 0; i < info.nparents; i++)
				{
					ProllyHash	pk = prolly_hash_key(&info.parents[i]);
					Height	   *ph = hash_search(heights, &pk, HASH_FIND, NULL);

					if (ph != NULL)
					{
						stack[top].height = Max(stack[top].height, ph->height + 1);
						continue;
					}
					/* (a parent not found yet: found first) */
					if (depth == cap)
					{
						cap *= 2;
						stack = repalloc_array(stack, HeightFrame, cap);
					}
					stack[depth++] = (HeightFrame) {pk, top, false, 1};
				}
				if (depth - 1 != top)
					continue;
			}
		}

		/* Found: each of its parents' has raised it, as it was found. */
		if (!known)
		{
			h = hash_search(heights, &stack[top].id, HASH_ENTER, NULL);
			h->height = stack[top].height;
		}
		if (stack[top].pusher >= 0)
		{
			HeightFrame *pusher = &stack[stack[top].pusher];

			pusher->height = Max(pusher->height, stack[top].height + 1);
		}
		else
			result = stack[top].height;
		depth--;
	}
	pfree(stack);
	return result;
}

/*
 * The best common ancestor of a and b: of those no other common ancestor
 * descends from (two or more, in a criss-cross merge's history), the one
 * of the greatest height, then the least id, as Dolt chooses, so the same
 * whichever way round they're asked for.  False if they have none.
 */
bool
prolly_history_merge_base(const ProllyHash *a, const ProllyHash *b,
						  ProllyCommitLookup lookup, void *lookup_arg,
						  ProllyHash *base)
{
	HTAB	   *ofa = id_set("prolly merge base");
	HTAB	   *beneath = NULL;
	HTAB	   *heights = NULL;
	ProllyHash	unused;
	ProllyHash *common;
	int			ncommon;
	int64		best_height = 0;
	bool		found = false;

	(void) walk_from(a, 1, lookup, lookup_arg, add_ancestor, ofa, &unused);
	ncommon = first_common(b, ofa, lookup, lookup_arg, &common);

	/*
	 * Of several, what's beneath one (its ancestors, itself aside) isn't
	 * the best: one walk from all their parents.
	 */
	if (ncommon > 1)
	{
		ProllyHash *parents = palloc_array(ProllyHash, ncommon * PROLLY_MAX_PARENTS);
		int			nparents = 0;

		for (int i = 0; i < ncommon; i++)
		{
			ProllyCommitInfo info;

			if (!lookup(&common[i], &info, lookup_arg))
				continue;
			for (int p = 0; p < info.nparents; p++)
				parents[nparents++] = info.parents[p];
		}
		beneath = id_set("prolly merge base, beneath another");
		(void) walk_from(parents, nparents, lookup, lookup_arg, add_ancestor,
						 beneath, &unused);
		heights = prolly_history_heights();
	}
	for (int i = 0; i < ncommon; i++)
	{
		ProllyHash *id = &common[i];
		int64		height = 0;

		if (beneath != NULL && hash_search(beneath, id, HASH_FIND, NULL) != NULL)
			continue;
		if (heights != NULL)
			height = prolly_history_height(id, heights, lookup, lookup_arg);
		if (!found || height > best_height ||
			(height == best_height &&
			 (id->len < base->len ||
			  (id->len == base->len && memcmp(id->bytes, base->bytes, id->len) < 0))))
		{
			*base = *id;
			best_height = height;
		}
		found = true;
	}
	if (heights)
		hash_destroy(heights);
	if (beneath)
		hash_destroy(beneath);
	hash_destroy(ofa);
	return found;
}
