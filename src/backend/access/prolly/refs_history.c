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
#include "access/prolly_window.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
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
 * The maximal common ancestors of a and b (those no other common ancestor
 * descends from) that may be the best, by heights instead of a walk of
 * all of a's ancestors: commits are taken highest first, each marked with
 * the side it was reached from (a's, b's, both), a commit reached from
 * both a candidate and its ancestors marked as beneath one, until every
 * commit still to be taken is beneath one or lower than the first
 * candidate.  A commit's height exceeds its parents', so none is taken
 * before a commit it descends from, and a commit's marks are all made by
 * the time it is taken: one reached from both sides and not beneath a
 * candidate then is a maximal one, and none lower than the first can be
 * the best.  The walk reads the commits above the merge base and their
 * parents, and holds marks for those alone, however far one side's
 * history reaches beneath the base.  Each candidate's height is returned
 * with it.  False, the caller walking instead, if a commit read has no
 * height recorded, or one no less than a child's (heights that aren't
 * Dolt's), or can't be read.  The edges it doesn't take are trusted: a
 * commit records its height as it is made, and one from a remote is
 * refused unless its height is its parents' (transfer_chunks.c).
 */
#define PAINT_A			0x01
#define PAINT_B			0x02
#define PAINT_BENEATH	0x04
#define PAINT_FOUND		0x08

typedef struct PaintFlags
{
	ProllyHash	id;				/* (the key) */
	uint8		flags;
	int			queued;			/* how many times it is in the queue */
} PaintFlags;

typedef struct PaintEntry
{
	ProllyHash	id;
	int64		height;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
} PaintEntry;

typedef struct PaintQueue
{
	PaintEntry *e;
	int			n;
	int			cap;
	int			open;			/* entries not beneath a candidate */
} PaintQueue;

static void
paint_push(PaintQueue *q, PaintFlags *pf, const ProllyHash *id,
		   const ProllyCommitInfo *info)
{
	int64		height = info->height;
	int			i;

	if (q->n == q->cap)
	{
		q->cap *= 2;
		q->e = repalloc_array(q->e, PaintEntry, q->cap);
	}
	i = q->n++;
	while (i > 0 && q->e[(i - 1) / 2].height < height)
	{
		q->e[i] = q->e[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	q->e[i].id = *id;
	q->e[i].height = height;
	q->e[i].nparents = info->nparents;
	memcpy(q->e[i].parents, info->parents, sizeof(ProllyHash) * info->nparents);
	pf->queued++;
	if (!(pf->flags & PAINT_BENEATH))
		q->open++;
}

static PaintEntry
paint_pop(PaintQueue *q)
{
	PaintEntry	top = q->e[0];
	PaintEntry	last = q->e[--q->n];
	int			i = 0;

	for (;;)
	{
		int			c = 2 * i + 1;

		if (c >= q->n)
			break;
		if (c + 1 < q->n && q->e[c + 1].height > q->e[c].height)
			c++;
		if (q->e[c].height <= last.height)
			break;
		q->e[i] = q->e[c];
		i = c;
	}
	if (q->n > 0)
		q->e[i] = last;
	return top;
}

static PaintFlags *
paint_flags(HTAB *flags, const ProllyHash *id)
{
	ProllyHash	key = prolly_hash_key(id);
	bool		found;
	PaintFlags *pf = hash_search(flags, &key, HASH_ENTER, &found);

	if (!found)
	{
		pf->flags = 0;
		pf->queued = 0;
	}
	return pf;
}

/* pf's marks gain add, the queue's entries of it counted as they now are */
static void
paint_mark(PaintQueue *q, PaintFlags *pf, uint8 add)
{
	if ((add & PAINT_BENEATH) && !(pf->flags & PAINT_BENEATH))
		q->open -= pf->queued;
	pf->flags |= add;
}

static bool
paint_common(const ProllyHash *a, const ProllyHash *b,
			 ProllyCommitLookup lookup, void *lookup_arg,
			 ProllyHash **found, int64 **heights, int *nfound)
{
	HASHCTL		ctl;
	HTAB	   *flags;
	PaintQueue	q;
	ProllyCommitInfo info;
	int			cap = 4;
	bool		ok = true;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(PaintFlags);
	ctl.hcxt = CurrentMemoryContext;
	flags = hash_create("prolly merge base paint", 256, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	q.cap = 64;
	q.n = 0;
	q.open = 0;
	q.e = palloc_array(PaintEntry, q.cap);
	*found = palloc_array(ProllyHash, cap);
	*heights = palloc_array(int64, cap);
	*nfound = 0;

	for (int side = 0; side < 2 && ok; side++)
	{
		const ProllyHash *id = side == 0 ? a : b;

		if (!lookup(id, &info, lookup_arg) || !info.has_height)
			ok = false;
		else
		{
			PaintFlags *pf = paint_flags(flags, id);

			paint_mark(&q, pf, side == 0 ? PAINT_A : PAINT_B);
			paint_push(&q, pf, id, &info);
		}
	}
	/*
	 * Once a candidate is found, every commit still to be taken that is
	 * lower than the first (the highest) can be no better a merge base, nor
	 * mark one it found as beneath another: the walk ends there, however
	 * far one side's commits reach beneath it.
	 */
	while (ok && q.open > 0 &&
		   !(*nfound > 0 && q.e[0].height < (*heights)[0]))
	{
		PaintEntry	e = paint_pop(&q);
		PaintFlags *f = paint_flags(flags, &e.id);
		uint8		down;

		CHECK_FOR_INTERRUPTS();
		f->queued--;
		if (!(f->flags & PAINT_BENEATH))
			q.open--;
		if ((f->flags & (PAINT_A | PAINT_B | PAINT_BENEATH)) == (PAINT_A | PAINT_B))
		{
			if (!(f->flags & PAINT_FOUND))
			{
				if (*nfound == cap)
				{
					cap *= 2;
					*found = repalloc_array(*found, ProllyHash, cap);
					*heights = repalloc_array(*heights, int64, cap);
				}
				(*found)[*nfound] = e.id;
				(*heights)[(*nfound)++] = e.height;
				f->flags |= PAINT_FOUND;
			}
			down = (f->flags & (PAINT_A | PAINT_B)) | PAINT_BENEATH;
		}
		else
			down = f->flags & (PAINT_A | PAINT_B | PAINT_BENEATH);
		for (int p = 0; p < e.nparents; p++)
		{
			ProllyCommitInfo pinfo;
			PaintFlags *pf = paint_flags(flags, &e.parents[p]);

			if ((pf->flags | down) == pf->flags)
				continue;
			paint_mark(&q, pf, down);
			if (!lookup(&e.parents[p], &pinfo, lookup_arg) || !pinfo.has_height ||
				pinfo.height >= e.height)
			{
				ok = false;
				break;
			}
			paint_push(&q, pf, &e.parents[p], &pinfo);
		}
	}
	pfree(q.e);
	hash_destroy(flags);
	if (!ok)
	{
		pfree(*found);
		pfree(*heights);
	}
	return ok;
}

/*
 * Whether a maximal common ancestor id (of height) is a better merge base
 * than base (of base_height): the greater height, then the least id, as
 * Dolt chooses, so the same whichever way round a and b are asked for.
 */
static bool
better_base(const ProllyHash *id, int64 height, const ProllyHash *base,
			int64 base_height)
{
	if (height != base_height)
		return height > base_height;
	if (id->len != base->len)
		return id->len < base->len;
	return memcmp(id->bytes, base->bytes, id->len) < 0;
}

/*
 * Of candidates common[] (ncommon of them, every maximal common ancestor
 * among them), the best: of those no other descends from, the one of the
 * greatest height, then the least id.  False if there are none.  (For
 * candidates found walking, whose heights may not be recorded: several
 * have every ancestor of theirs walked to find those beneath another.)
 */
static bool
best_common(const ProllyHash *common, int ncommon, ProllyCommitLookup lookup,
			void *lookup_arg, ProllyHash *base)
{
	HTAB	   *beneath = NULL;
	HTAB	   *heights = NULL;
	ProllyHash	unused;
	int64		best_height = 0;
	bool		found = false;

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
		const ProllyHash *id = &common[i];
		int64		height = 0;

		if (beneath != NULL && hash_search(beneath, id, HASH_FIND, NULL) != NULL)
			continue;
		if (heights != NULL)
			height = prolly_history_height(id, heights, lookup, lookup_arg);
		if (!found || better_base(id, height, base, best_height))
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
	return found;
}

/* The candidates first_common() finds, by a walk of all of a's ancestors. */
static int
walked_common(const ProllyHash *a, const ProllyHash *b,
			  ProllyCommitLookup lookup, void *lookup_arg, ProllyHash **common)
{
	HTAB	   *ofa = id_set("prolly merge base");
	ProllyHash	unused;
	int			n;

	(void) walk_from(a, 1, lookup, lookup_arg, add_ancestor, ofa, &unused);
	n = first_common(b, ofa, lookup, lookup_arg, common);
	hash_destroy(ofa);
	return n;
}

/*
 * Every maximal common ancestor of a and b (the candidates a merge base is
 * chosen from: several where histories cross), into *bases (palloc'd);
 * the count.  Where heights aren't recorded, the commits a walk finds
 * common first, which may hold some beneath others.
 */
int
prolly_history_merge_bases(const ProllyHash *a, const ProllyHash *b,
						   ProllyCommitLookup lookup, void *lookup_arg,
						   ProllyHash **bases)
{
	int64	   *heights;
	int			n;

	if (paint_common(a, b, lookup, lookup_arg, bases, &heights, &n))
	{
		pfree(heights);
		return n;
	}
	return walked_common(a, b, lookup, lookup_arg, bases);
}

bool
prolly_history_merge_base(const ProllyHash *a, const ProllyHash *b,
						  ProllyCommitLookup lookup, void *lookup_arg,
						  ProllyHash *base)
{
	ProllyHash *common;
	int64	   *heights;
	int			ncommon;
	bool		found = false;
	int64		best_height = 0;

	if (!paint_common(a, b, lookup, lookup_arg, &common, &heights, &ncommon))
	{
		ncommon = walked_common(a, b, lookup, lookup_arg, &common);
		return best_common(common, ncommon, lookup, lookup_arg, base);
	}

	/*
	 * Every candidate is a maximal one, its height recorded: none is
	 * beneath another, so nothing beneath them is walked to find out.
	 */
	for (int i = 0; i < ncommon; i++)
	{
		if (!found || better_base(&common[i], heights[i], base, best_height))
		{
			*base = common[i];
			best_height = heights[i];
		}
		found = true;
	}
#ifdef USE_ASSERT_CHECKING
	{
		/*
		 * (the walk of all of a's ancestors finds the same; the commits it
		 * reads aren't counted)
		 */
		PgStat_Counter read = PendingProllyStats[PGSTAT_PROLLY_COMMITS_READ];
		ProllyHash *walked;
		ProllyHash	other;
		int			nwalked = walked_common(a, b, lookup, lookup_arg, &walked);

		Assert(best_common(walked, nwalked, lookup, lookup_arg, &other) == found);
		Assert(!found || (other.len == base->len &&
						  memcmp(other.bytes, base->bytes, base->len) == 0));
		PendingProllyStats[PGSTAT_PROLLY_COMMITS_READ] = read;
	}
#endif
	return found;
}
