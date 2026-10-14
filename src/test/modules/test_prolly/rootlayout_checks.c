/*--------------------------------------------------------------------------
 *
 * rootlayout_checks.c
 *		Conformance checks for the root layout interface.
 *
 * Layouts are built from the prolly_d1 profile's tree implementation and
 * foundation components, over a set of real relation trees, and compared
 * against an in-memory reference: for each sub-root and each of NREL
 * relnumbers, which relation tree it holds, or none.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/rootlayout_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "access/xact.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "test_prolly.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#define NREL		200			/* relnumbers per sub-root */
#define NTREES		12			/* distinct relation trees */
#define ABSENT		(-1)
#define NSLOTS		(PROLLY_NUM_SUBROOTS * NREL)

/* Relids: catalog OIDs below FirstNormalObjectId, user ones above. */
static RelFileNumber
relnum_of(int sub, int i)
{
	return sub == PROLLY_SUBROOT_CATALOG ? 1000 + 3 * i : 16384 + 7 * i;
}

typedef struct Fixture
{
	const ProllyRootLayoutRoutine *rl;
	ProllyTreeEnv tenv;
	ProllyRootEnv env;
	ProllyNodeRef trees[NTREES];
	ProllyNodeRef empty_tree;
} Fixture;

/* Reference state: slot sub * NREL + i holds a tree number or ABSENT. */
typedef struct Ref
{
	int			tree[NSLOTS];
} Ref;

static int
memcmp_keys(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

static bool
same_hash(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	return a->hash.len == b->hash.len &&
		memcmp(a->hash.bytes, b->hash.bytes, a->hash.len) == 0;
}

/* Relation tree j: keys 0 .. size-1, values naming j; some span many nodes. */
static void
build_tree(Fixture *f, int j, ProllyNodeRef *root)
{
	const ProllyTreeRoutine *tr = f->env.tree;
	ProllyTreeBuilder *b = PROLLY_CALL(tr, build_begin, &f->tenv, CurrentMemoryContext);
	int			size = j == 0 ? 0 : (j % 3 == 0 ? 600 : 5 * j);
	StringInfoData v;

	initStringInfo(&v);
	for (int k = 0; k < size; k++)
	{
		uint8		kb[4] = {(uint8) (k >> 24), (uint8) (k >> 16), (uint8) (k >> 8), (uint8) k};
		ProllyKey	key = {kb, 4};
		ProllyValue value;

		resetStringInfo(&v);
		appendStringInfo(&v, "tree %d key %d %0*d", j, k, 20 + (k % 60), 0);
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(tr, build_add, b, &key, &value);
	}
	pfree(v.data);
	PROLLY_CALL(tr, build_finish, b, root);
}

static void
make_fixture(const ProllyImplInfo *impl, Fixture *f)
{
	ProllyComponents c;

	memset(f, 0, sizeof(*f));
	f->rl = (const ProllyRootLayoutRoutine *) impl;
	prolly_components_for_am("prolly_d1", &c);
	f->tenv.node_format = c.node_format;
	f->tenv.chunker = c.chunker;
	f->tenv.hash = c.hash;
	f->tenv.node_store = c.node_store;
	f->tenv.log = c.log;
	PROLLY_CALL(f->tenv.node_store, create, NULL, NULL, RELPERSISTENCE_PERMANENT);
	f->tenv.store = PROLLY_CALL(f->tenv.node_store, open, NULL);
	f->tenv.compare = memcmp_keys;
	f->env.tree = c.tree;
	f->env.tree_env = &f->tenv;
	for (int j = 0; j < NTREES; j++)
		build_tree(f, j, &f->trees[j]);
	f->empty_tree = f->trees[0];
}

static void
ref_clear(Ref *ref)
{
	for (int s = 0; s < NSLOTS; s++)
		ref->tree[s] = ABSENT;
}

/* Apply the change from *ref to *target, as one batch, updating *ref. */
static void
apply_to(Fixture *f, Ref *ref, const Ref *target, ProllyNodeRef *root)
{
	ProllyRootEdit *edits = palloc(sizeof(ProllyRootEdit) * NSLOTS);
	int			n = 0;
	ProllyNodeRef newroot;

	for (int s = 0; s < NSLOTS; s++)
	{
		if (ref->tree[s] == target->tree[s])
			continue;
		edits[n].sub = (ProllySubRoot) (s / NREL);
		edits[n].relnumber = relnum_of(s / NREL, s % NREL);
		edits[n].is_remove = target->tree[s] == ABSENT;
		if (!edits[n].is_remove)
			edits[n].tree = f->trees[target->tree[s]];
		n++;
		ref->tree[s] = target->tree[s];
	}
	PROLLY_CALL(f->rl, apply, &f->env, root, edits, n, &newroot);
	*root = newroot;
	pfree(edits);
}

/* A random change of up to maxn slots to *ref, applied as one batch. */
static void
random_batch(Fixture *f, pg_prng_state *rng, int maxn, Ref *ref, ProllyNodeRef *root)
{
	Ref			target = *ref;
	int			n = (int) pg_prng_uint64_range(rng, 1, maxn);

	for (int i = 0; i < n; i++)
	{
		int			s = (int) pg_prng_uint64_range(rng, 0, NSLOTS - 1);

		target.tree[s] = pg_prng_uint64_range(rng, 0, 3) == 0 ? ABSENT :
			(int) pg_prng_uint64_range(rng, 0, NTREES - 1);
	}
	apply_to(f, ref, &target, root);
}

typedef struct ScanSeen
{
	const Fixture *f;
	const Ref  *ref;
	int			sub;
	int			next;			/* next slot index i expected at or after */
	int			n;
} ScanSeen;

static void
check_scanned(ProllySubRoot sub, RelFileNumber relnumber, const ProllyNodeRef *tree, void *arg)
{
	ScanSeen   *s = arg;

	if ((int) sub != s->sub)
		elog(ERROR, "scan of sub-root %d visited sub-root %d", s->sub, (int) sub);
	while (s->next < NREL && s->ref->tree[s->sub * NREL + s->next] == ABSENT)
		s->next++;
	if (s->next >= NREL || relnum_of(s->sub, s->next) != relnumber)
		elog(ERROR, "scan of sub-root %d returned relnumber %u out of order or absent",
			 s->sub, relnumber);
	if (!same_hash(tree, &s->f->trees[s->ref->tree[s->sub * NREL + s->next]]))
		elog(ERROR, "scan returned the wrong tree for relnumber %u", relnumber);
	s->next++;
	s->n++;
}

/* Lookup and scan agree with ref. */
static void
check_matches(Fixture *f, const ProllyNodeRef *root, const Ref *ref, const char *when)
{
	for (int sub = 0; sub < PROLLY_NUM_SUBROOTS; sub++)
	{
		ScanSeen	seen = {f, ref, sub, 0, 0};
		int			want = 0;

		for (int i = 0; i < NREL; i++)
		{
			int			t = ref->tree[sub * NREL + i];
			ProllyNodeRef got;
			bool		found = PROLLY_CALL(f->rl, lookup, &f->env, root,
											(ProllySubRoot) sub, relnum_of(sub, i), &got);

			if (found != (t != ABSENT))
				elog(ERROR, "%s: lookup of relnumber %u in sub-root %d %s", when,
					 relnum_of(sub, i), sub, found ? "found an absent relation" : "missed a relation");
			if (found && !same_hash(&got, &f->trees[t]))
				elog(ERROR, "%s: lookup of relnumber %u returned the wrong tree", when,
					 relnum_of(sub, i));
			if (t != ABSENT)
				want++;
		}
		PROLLY_CALL(f->rl, scan, &f->env, root, (ProllySubRoot) sub, check_scanned, &seen);
		if (seen.n != want)
			elog(ERROR, "%s: scan of sub-root %d found %d relations, expected %d",
				 when, sub, seen.n, want);
	}
}

void
rootlayout_empty(const ProllyImplInfo *impl)
{
	Fixture		f;
	ProllyNodeRef a;
	ProllyNodeRef b;
	Ref			ref;

	make_fixture(impl, &f);
	PROLLY_CALL(f.rl, empty, &f.env, &a);
	PROLLY_CALL(f.rl, empty, &f.env, &b);
	if (!same_hash(&a, &b))
		elog(ERROR, "two empty roots have different hashes");
	for (int sub = 0; sub < PROLLY_NUM_SUBROOTS; sub++)
	{
		ProllyNodeRef s;

		PROLLY_CALL(f.rl, subroot, &f.env, &a, (ProllySubRoot) sub, &s);
		if (!same_hash(&s, &f.empty_tree))
			elog(ERROR, "sub-root %d of the empty root is not the empty tree", sub);
	}
	ref_clear(&ref);
	check_matches(&f, &a, &ref, "empty root");

	/* An empty batch, and removing absent relations, change nothing. */
	{
		ProllyRootEdit e;

		memset(&e, 0, sizeof(e));
		e.sub = PROLLY_SUBROOT_USER_DATA;
		e.relnumber = relnum_of(PROLLY_SUBROOT_USER_DATA, 0);
		e.is_remove = true;
		PROLLY_CALL(f.rl, apply, &f.env, &a, NULL, 0, &b);
		if (!same_hash(&a, &b))
			elog(ERROR, "an empty batch changed the root");
		PROLLY_CALL(f.rl, apply, &f.env, &a, &e, 1, &b);
		if (!same_hash(&a, &b))
			elog(ERROR, "removing an absent relation changed the root");
	}
}

void
rootlayout_apply_matches_reference(const ProllyImplInfo *impl)
{
	Fixture		f;
	Ref			ref;
	ProllyNodeRef root;
	pg_prng_state rng;

	make_fixture(impl, &f);
	pg_prng_seed(&rng, 20261013);
	ref_clear(&ref);
	PROLLY_CALL(f.rl, empty, &f.env, &root);

	for (int batch = 0; batch < 40; batch++)
	{
		CHECK_FOR_INTERRUPTS();
		random_batch(&f, &rng, batch % 5 == 0 ? 300 : 12, &ref, &root);
		check_matches(&f, &root, &ref, psprintf("after batch %d", batch));
	}
}

void
rootlayout_history_independent(const ProllyImplInfo *impl)
{
	Fixture		f;
	Ref			target;
	Ref			ref;
	ProllyNodeRef empty;
	ProllyNodeRef direct;
	pg_prng_state rng;

	make_fixture(impl, &f);
	PROLLY_CALL(f.rl, empty, &f.env, &empty);

	pg_prng_seed(&rng, 42);
	for (int s = 0; s < NSLOTS; s++)
		target.tree[s] = pg_prng_uint64_range(&rng, 0, 2) == 0 ? ABSENT :
			(int) pg_prng_uint64_range(&rng, 0, NTREES - 1);

	/* In one batch from empty. */
	ref_clear(&ref);
	direct = empty;
	apply_to(&f, &ref, &target, &direct);

	for (int history = 0; history < 3; history++)
	{
		ProllyNodeRef root = empty;

		ref_clear(&ref);
		pg_prng_seed(&rng, 1000 + history);
		for (int batch = 0; batch < 15; batch++)
			random_batch(&f, &rng, history == 0 ? 400 : 30, &ref, &root);

		/* Then reach the target a few slots at a time. */
		for (int start = 0; start < NSLOTS;)
		{
			int			size = (int) pg_prng_uint64_range(&rng, 1, 90);
			Ref			step = ref;

			for (int s = start; s < Min(start + size, NSLOTS); s++)
				step.tree[s] = target.tree[s];
			apply_to(&f, &ref, &step, &root);
			start += size;
		}

		check_matches(&f, &root, &target, psprintf("history %d", history));
		if (!same_hash(&root, &direct))
			elog(ERROR, "history %d reached the same relations with a different root hash",
				 history);
	}

	/* Removing every relation gives the empty root. */
	{
		Ref			none;

		ref_clear(&none);
		ref = target;
		apply_to(&f, &ref, &none, &direct);
		if (!same_hash(&direct, &empty))
			elog(ERROR, "removing every relation did not give the empty root's hash");
	}
}

void
rootlayout_subroots_independent(const ProllyImplInfo *impl)
{
	Fixture		f;
	Ref			ref;
	ProllyNodeRef root;
	pg_prng_state rng;

	make_fixture(impl, &f);
	pg_prng_seed(&rng, 7);
	ref_clear(&ref);
	PROLLY_CALL(f.rl, empty, &f.env, &root);
	random_batch(&f, &rng, 300, &ref, &root);

	for (int sub = 0; sub < PROLLY_NUM_SUBROOTS; sub++)
	{
		ProllyNodeRef before[PROLLY_NUM_SUBROOTS];
		ProllyNodeRef after[PROLLY_NUM_SUBROOTS];
		ProllyNodeRef old = root;
		Ref			target = ref;
		int			slot = sub * NREL + 5;

		for (int s = 0; s < PROLLY_NUM_SUBROOTS; s++)
			PROLLY_CALL(f.rl, subroot, &f.env, &root, (ProllySubRoot) s, &before[s]);

		/* Change one relation in sub. */
		target.tree[slot] = (ref.tree[slot] + 2) % NTREES;
		apply_to(&f, &ref, &target, &root);
		for (int s = 0; s < PROLLY_NUM_SUBROOTS; s++)
			PROLLY_CALL(f.rl, subroot, &f.env, &root, (ProllySubRoot) s, &after[s]);

		if (same_hash(&before[sub], &after[sub]))
			elog(ERROR, "changing a relation in sub-root %d left it unchanged", sub);
		for (int other = 0; other < PROLLY_NUM_SUBROOTS; other++)
			if (other != sub && !same_hash(&before[other], &after[other]))
				elog(ERROR, "changing a relation in sub-root %d changed sub-root %d",
					 sub, other);
		if (same_hash(&old, &root))
			elog(ERROR, "changing a relation left the branch root unchanged");

		/* Setting a relation to the tree it already has changes nothing. */
		{
			ProllyRootEdit e;
			ProllyNodeRef again;

			e.sub = (ProllySubRoot) sub;
			e.relnumber = relnum_of(sub, 5);
			e.tree = f.trees[ref.tree[slot]];
			e.is_remove = false;
			PROLLY_CALL(f.rl, apply, &f.env, &root, &e, 1, &again);
			if (!same_hash(&again, &root))
				elog(ERROR, "setting a relation to its own tree changed the root");
		}
	}
}

/*
 * apply() of one edit, in its own subtransaction: the error message if it
 * failed, or NULL if it succeeded.
 */
static char *
apply_error(Fixture *f, const ProllyNodeRef *root, const ProllyRootEdit *e)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	char	   *message = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcxt);
	PG_TRY();
	{
		ProllyNodeRef newroot;

		PROLLY_CALL(f->rl, apply, &f->env, root, e, 1, &newroot);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
		message = edata->message;
	}
	PG_END_TRY();
	return message;
}

/*
 * A reference whose hash length is 0 or too long is refused, so a root
 * never holds one that lookup() can't read back.
 */
void
rootlayout_apply_rejects_invalid_refs(const ProllyImplInfo *impl)
{
	Fixture		f;
	ProllyNodeRef root;
	static const int bad_lengths[] = {0, PROLLY_HASH_MAX_LEN + 1, 255};

	make_fixture(impl, &f);
	PROLLY_CALL(f.rl, empty, &f.env, &root);

	for (int sub = 0; sub < PROLLY_NUM_SUBROOTS; sub++)
	{
		for (int b = 0; b < lengthof(bad_lengths); b++)
		{
			ProllyRootEdit e;
			char	   *message;

			/* A real tree's reference, with its block hint, but a bad hash. */
			memset(&e, 0, sizeof(e));
			e.sub = (ProllySubRoot) sub;
			e.relnumber = relnum_of(sub, 1);
			e.tree = f.trees[3];
			e.tree.hash.len = (uint8) bad_lengths[b];
			e.is_remove = false;

			message = apply_error(&f, &root, &e);
			if (message == NULL)
				elog(ERROR, "apply accepted a reference with hash length %d in sub-root %d",
					 bad_lengths[b], sub);
			if (strstr(message, "invalid hash length") == NULL)
				elog(ERROR, "apply refused hash length %d for the wrong reason: %s",
					 bad_lengths[b], message);
		}
	}

	/* The root is still usable, and a valid reference is accepted. */
	{
		ProllyRootEdit e;
		ProllyNodeRef newroot;
		ProllyNodeRef got;

		memset(&e, 0, sizeof(e));
		e.sub = PROLLY_SUBROOT_USER_DATA;
		e.relnumber = relnum_of(PROLLY_SUBROOT_USER_DATA, 1);
		e.tree = f.trees[3];
		PROLLY_CALL(f.rl, apply, &f.env, &root, &e, 1, &newroot);
		if (!PROLLY_CALL(f.rl, lookup, &f.env, &newroot, e.sub, e.relnumber, &got) ||
			!same_hash(&got, &f.trees[3]))
			elog(ERROR, "a valid reference was not stored after rejected ones");
	}
}

typedef struct DiffSeen
{
	const Fixture *f;
	const Ref  *from;
	const Ref  *to;
	int			last;			/* last slot reported */
	int			n;
} DiffSeen;

static int
slot_of(ProllySubRoot sub, RelFileNumber relnumber)
{
	for (int i = 0; i < NREL; i++)
		if (relnum_of(sub, i) == relnumber)
			return sub * NREL + i;
	elog(ERROR, "diff reported unknown relnumber %u", relnumber);
	return -1;					/* keep compiler quiet */
}

static void
check_change(ProllyDiffKind kind, ProllySubRoot sub, RelFileNumber relnumber,
			 const ProllyNodeRef *before, const ProllyNodeRef *after, void *arg)
{
	DiffSeen   *d = arg;
	int			s = slot_of(sub, relnumber);
	int			ft = d->from->tree[s];
	int			tt = d->to->tree[s];

	if (s <= d->last)
		elog(ERROR, "diff reported relnumber %u out of (sub-root, relnumber) order", relnumber);
	d->last = s;
	d->n++;
	switch (kind)
	{
		case PROLLY_DIFF_ADDED:
			if (ft != ABSENT || tt == ABSENT || before != NULL ||
				!same_hash(after, &d->f->trees[tt]))
				elog(ERROR, "diff: relnumber %u wrongly reported added", relnumber);
			break;
		case PROLLY_DIFF_REMOVED:
			if (ft == ABSENT || tt != ABSENT || after != NULL ||
				!same_hash(before, &d->f->trees[ft]))
				elog(ERROR, "diff: relnumber %u wrongly reported removed", relnumber);
			break;
		case PROLLY_DIFF_MODIFIED:
			if (ft == ABSENT || tt == ABSENT ||
				same_hash(&d->f->trees[ft], &d->f->trees[tt]) ||
				!same_hash(before, &d->f->trees[ft]) ||
				!same_hash(after, &d->f->trees[tt]))
				elog(ERROR, "diff: relnumber %u wrongly reported modified", relnumber);
			break;
	}
}

void
rootlayout_diff_reports_changes(const ProllyImplInfo *impl)
{
	Fixture		f;
	Ref			from;
	Ref			to;
	ProllyNodeRef rfrom;
	ProllyNodeRef rto;
	pg_prng_state rng;

	make_fixture(impl, &f);
	pg_prng_seed(&rng, 99);
	ref_clear(&from);
	PROLLY_CALL(f.rl, empty, &f.env, &rfrom);
	random_batch(&f, &rng, 300, &from, &rfrom);

	for (int round = 0; round < 6; round++)
	{
		DiffSeen	seen = {&f, &from, &to, -1, 0};
		int			want = 0;

		to = from;
		rto = rfrom;
		/* Even rounds change one sub-root only, so the other is skipped. */
		if (round % 2 == 0)
		{
			Ref			target = to;
			int			sub = round / 2 % PROLLY_NUM_SUBROOTS;

			for (int i = 0; i < NREL; i += 9)
				target.tree[sub * NREL + i] = (i + round) % 4 == 0 ? ABSENT :
					(int) ((i + round) % NTREES);
			apply_to(&f, &to, &target, &rto);
		}
		else
			random_batch(&f, &rng, 40, &to, &rto);

		for (int s = 0; s < NSLOTS; s++)
		{
			int			ft = from.tree[s];
			int			tt = to.tree[s];

			if (ft == tt)
				continue;
			if (ft != ABSENT && tt != ABSENT && same_hash(&f.trees[ft], &f.trees[tt]))
				continue;
			want++;
		}
		PROLLY_CALL(f.rl, diff, &f.env, &rfrom, &rto, check_change, &seen);
		if (seen.n != want)
			elog(ERROR, "round %d: diff reported %d changes, expected %d", round,
				 seen.n, want);
	}

	/* A root has no differences from itself. */
	{
		DiffSeen	seen = {&f, &from, &from, -1, 0};

		PROLLY_CALL(f.rl, diff, &f.env, &rfrom, &rfrom, check_change, &seen);
		if (seen.n != 0)
			elog(ERROR, "a root differs from itself");
	}
}

/* Collects node hashes during a walk. */
typedef struct HashSet
{
	ProllyHash *hashes;
	int			n;
	int			max;
} HashSet;

static void
collect_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	HashSet    *s = arg;

	if (s->n == s->max)
	{
		s->max = Max(64, s->max * 2);
		s->hashes = s->hashes ? repalloc(s->hashes, sizeof(ProllyHash) * s->max) :
			palloc(sizeof(ProllyHash) * s->max);
	}
	s->hashes[s->n++] = node->hash;
}

static bool
in_set(const HashSet *s, const ProllyHash *h)
{
	for (int i = 0; i < s->n; i++)
		if (s->hashes[i].len == h->len && memcmp(s->hashes[i].bytes, h->bytes, h->len) == 0)
			return true;
	return false;
}

/*
 * Every node a root depends on is visited: the branch root, every node of
 * both sub-root trees (not just their top nodes), and every node of every
 * relation tree.  This is what reclamation must keep.
 */
void
rootlayout_walk_reaches_trees(const ProllyImplInfo *impl)
{
	Fixture		f;
	Ref			ref;
	Ref			full;
	ProllyNodeRef root;
	HashSet		all = {0};

	make_fixture(impl, &f);

	/* Every slot full, so each sub-root tree spans several nodes. */
	ref_clear(&ref);
	for (int s = 0; s < NSLOTS; s++)
		full.tree[s] = s % NTREES;
	PROLLY_CALL(f.rl, empty, &f.env, &root);
	apply_to(&f, &ref, &full, &root);
	PROLLY_CALL(f.rl, walk, &f.env, &root, collect_node, &all);

	if (!in_set(&all, &root.hash))
		elog(ERROR, "walk did not visit the branch root");

	/* The layout's own trees; walking them needs no key order. */
	for (int sub = 0; sub < PROLLY_NUM_SUBROOTS; sub++)
	{
		ProllyNodeRef s;
		HashSet		nodes = {0};

		PROLLY_CALL(f.rl, subroot, &f.env, &root, (ProllySubRoot) sub, &s);
		PROLLY_CALL(f.env.tree, walk, &f.tenv, &s, collect_node, &nodes);
		if (nodes.n < 2)
			elog(ERROR, "sub-root %d has only %d node; the test needs a multi-node mapping tree",
				 sub, nodes.n);
		for (int i = 0; i < nodes.n; i++)
			if (!in_set(&all, &nodes.hashes[i]))
				elog(ERROR, "walk missed node %d of %d of sub-root %d", i, nodes.n, sub);
	}

	/* Every node of every relation tree present. */
	for (int j = 0; j < NTREES; j++)
	{
		HashSet		nodes = {0};

		PROLLY_CALL(f.env.tree, walk, &f.tenv, &f.trees[j], collect_node, &nodes);
		for (int i = 0; i < nodes.n; i++)
			if (!in_set(&all, &nodes.hashes[i]))
				elog(ERROR, "walk missed a node of relation tree %d", j);
	}
}
