/*
 * vc.c
 *	  Root values, commits, branches, merge and remotes.
 *
 * Schema and commit layouts are in chunkfmt.h.  A root value is a map from
 * table name to (schema hash, data root hash), both held strongly.
 *
 * Holds: every commit is held by a root ref (history is kept whole until
 * the window decides otherwise), and each branch holds its working root.
 * Mutating repo calls release the context's pins once results are held.
 */
#include <sys/types.h>
#include <time.h>

#include "vc.h"

#define ENTRY_LEN (2 * PHASH_LEN)
#define RV_VREFS 2				/* a table entry holds its schema and data root */

/*
 * The version-control layer does not yet handle history cut off by the
 * window, so a tree error here is fatal.
 */
static void
vc_must(ProllyCtx *ctx, int rc)
{
	char		hex[PHASH_HEX_LEN + 1];

	if (rc >= 0)
		return;
	phash_hex(&ctx->err_hash, hex);
	fprintf(stderr, "vc: tree error %d at chunk %s\n", rc, hex);
	abort();
}

/* ---------------------------------------------------------- root values */

static void
entry_encode(uint8_t out[ENTRY_LEN], const VcTable *t)
{
	memcpy(out, t->schema.b, PHASH_LEN);
	memcpy(out + PHASH_LEN, t->data.b, PHASH_LEN);
}

static void
entry_decode(const uint8_t *v, size_t vl, VcTable *t)
{
	if (vl != ENTRY_LEN)
	{
		fprintf(stderr, "vc: corrupt table entry\n");
		abort();
	}
	memcpy(t->schema.b, v, PHASH_LEN);
	memcpy(t->data.b, v + PHASH_LEN, PHASH_LEN);
}

PHash
vc_root_empty(ProllyCtx *ctx)
{
	PHash		h;

	vc_must(ctx, prolly_empty(ctx, RV_VREFS, &h));
	return h;
}

bool
vc_table_get(ProllyCtx *ctx, const PHash *rv, const char *name, VcTable *out)
{
	const uint8_t *v;
	size_t		vl;

	int			rc = prolly_get(ctx, rv, (const uint8_t *) name, strlen(name), &v, &vl);

	if (rc == PROLLY_NOT_FOUND)
		return false;
	vc_must(ctx, rc);
	entry_decode(v, vl, out);
	return true;
}

static PHash
put_entry(ProllyCtx *ctx, const PHash *rv, const char *name, const VcTable *t)
{
	uint8_t		val[ENTRY_LEN];
	ProllyEdit	e = {(const uint8_t *) name, strlen(name), val, ENTRY_LEN, false};

	PHash		out;

	entry_encode(val, t);
	vc_must(ctx, prolly_apply(ctx, rv, &e, 1, &out));
	return out;
}

static PHash
put_schema(ProllyCtx *ctx, const void *schema, size_t slen)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_SCHEMA);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, schema, slen);
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

const uint8_t *
vc_schema_get(ProllyCtx *ctx, const PHash *schema, size_t *len)
{
	size_t		l;
	const uint8_t *d = cs_get(ctx->cs, schema, &l);

	if (d == NULL || l < 2 || d[0] != CHUNK_TAG_SCHEMA ||
		d[1] != CHUNK_FORMAT_VERSION)
		return NULL;
	*len = l - 2;
	return d + 2;
}

int
vc_table_create(ProllyCtx *ctx, const PHash *rv, const char *name,
				const void *schema, size_t slen, PHash *out)
{
	VcTable		t;

	if (vc_table_get(ctx, rv, name, &t))
		return -1;
	t.schema = put_schema(ctx, schema, slen);
	vc_must(ctx, prolly_empty(ctx, 0, &t.data));
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

int
vc_table_alter(ProllyCtx *ctx, const PHash *rv, const char *name,
			   const void *schema, size_t slen, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	t.schema = put_schema(ctx, schema, slen);
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

int
vc_table_drop(ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out)
{
	VcTable		t;
	ProllyEdit	e = {(const uint8_t *) name, strlen(name), NULL, 0, true};

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	vc_must(ctx, prolly_apply(ctx, rv, &e, 1, out));
	return 0;
}

int
vc_table_edit(ProllyCtx *ctx, const PHash *rv, const char *name,
			  const ProllyEdit *edits, size_t n, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	vc_must(ctx, prolly_apply(ctx, &t.data, edits, n, &t.data));
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

int
vc_table_set_data(ProllyCtx *ctx, const PHash *rv, const char *name,
				  const PHash *data, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	t.data = *data;
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

typedef struct DiffTablesArg
{
	VcTableChangeFn fn;
	void	   *arg;
} DiffTablesArg;

static void
diff_tables_fn(void *arg, const ProllyDiff *d)
{
	DiffTablesArg *a = arg;
	VcTableChange c;

	memset(&c, 0, sizeof(c));
	c.name = d->key;
	c.nlen = d->klen;
	if ((c.has_from = d->from != NULL))
		entry_decode(d->from, d->flen, &c.from);
	if ((c.has_to = d->to != NULL))
		entry_decode(d->to, d->tlen, &c.to);
	a->fn(a->arg, &c);
}

void
vc_diff_tables(ProllyCtx *ctx, const PHash *from, const PHash *to,
			   VcTableChangeFn fn, void *arg)
{
	DiffTablesArg a = {fn, arg};

	vc_must(ctx, prolly_diff(ctx, from, to, diff_tables_fn, &a));
}

typedef struct ConflictList
{
	VcConflict *v;
	size_t		n;
	size_t		cap;
} ConflictList;

static void
add_conflict(ConflictList *l, VcConflictKind kind, const uint8_t *name,
			 size_t nlen, const uint8_t *key, size_t klen)
{
	VcConflict *c;

	px_grow((void **) &l->v, &l->cap, l->n + 1, sizeof(VcConflict));
	c = &l->v[l->n++];
	c->kind = kind;
	c->table = px_malloc(nlen + 1);
	memcpy(c->table, name, nlen);
	c->table[nlen] = '\0';
	c->key = key ? px_memdup(key, klen) : NULL;
	c->klen = key ? klen : 0;
}

/*
 * Merges one table both sides changed.  Returns false (after recording the
 * conflict) when no merged entry can be produced.
 */
static bool
merge_table(ProllyCtx *ctx, const ProllyDiff *o, const ProllyDiff *t,
			ConflictList *cl, VcTable *merged)
{
	VcTable		base,
				ours,
				theirs;
	bool		has_base = t->from != NULL;
	ProllyMergeResult mr;
	PHash		empty;

	if (o->to == NULL || t->to == NULL)
	{
		add_conflict(cl, VC_CONFLICT_TABLE, t->key, t->klen, NULL, 0);
		return false;
	}
	entry_decode(o->to, o->tlen, &ours);
	entry_decode(t->to, t->tlen, &theirs);
	if (has_base)
		entry_decode(t->from, t->flen, &base);

	if (phash_eq(&ours.schema, &theirs.schema))
		merged->schema = ours.schema;
	else if (has_base && phash_eq(&theirs.schema, &base.schema))
		merged->schema = ours.schema;
	else if (has_base && phash_eq(&ours.schema, &base.schema))
		merged->schema = theirs.schema;
	else
	{
		add_conflict(cl, VC_CONFLICT_SCHEMA, t->key, t->klen, NULL, 0);
		return false;
	}

	vc_must(ctx, prolly_empty(ctx, 0, &empty));
	vc_must(ctx, prolly_merge(ctx, has_base ? &base.data : &empty, &ours.data,
							  &theirs.data, &mr));
	for (size_t i = 0; i < mr.nconflicts; i++)
		add_conflict(cl, VC_CONFLICT_ROW, t->key, t->klen,
					 mr.conflicts[i].key, mr.conflicts[i].klen);
	merged->data = mr.root;
	prolly_merge_result_free(&mr);
	return true;
}

void
vc_merge_roots(ProllyCtx *ctx, const PHash *base, const PHash *ours,
			   const PHash *theirs, PHash *out,
			   VcConflict **conflicts, size_t *nconflicts)
{
	ProllyDiffList dours,
				dtheirs;
	ConflictList cl = {0};
	ProllyEdit *edits;
	uint8_t    *vals;
	size_t		nedits = 0,
				a = 0;

	vc_must(ctx, prolly_diff_collect(ctx, base, ours, &dours));
	vc_must(ctx, prolly_diff_collect(ctx, base, theirs, &dtheirs));
	edits = px_malloc(dtheirs.n * sizeof(ProllyEdit));
	vals = px_malloc(dtheirs.n * ENTRY_LEN);
	for (size_t b = 0; b < dtheirs.n; b++)
	{
		const ProllyDiff *t = &dtheirs.v[b];
		const ProllyDiff *o = NULL;
		ProllyEdit *e = &edits[nedits];
		VcTable		merged;

		while (a < dours.n &&
			   key_cmp(dours.v[a].key, dours.v[a].klen, t->key, t->klen) < 0)
			a++;
		if (a < dours.n &&
			key_cmp(dours.v[a].key, dours.v[a].klen, t->key, t->klen) == 0)
			o = &dours.v[a];

		e->key = t->key;
		e->klen = t->klen;
		if (o == NULL)
		{
			/* only theirs changed this table */
			e->val = t->to;
			e->vlen = t->tlen;
			e->del = t->to == NULL;
			nedits++;
		}
		else if (o->to != NULL && t->to != NULL &&
				 bytes_eq(o->to, o->tlen, t->to, t->tlen))
			continue;
		else if (o->to == NULL && t->to == NULL)
			continue;
		else if (merge_table(ctx, o, t, &cl, &merged))
		{
			entry_encode(vals + nedits * ENTRY_LEN, &merged);
			e->val = vals + nedits * ENTRY_LEN;
			e->vlen = ENTRY_LEN;
			e->del = false;
			nedits++;
		}
	}
	*out = *ours;
	if (nedits)
		vc_must(ctx, prolly_apply(ctx, ours, edits, nedits, out));
	*conflicts = cl.v;
	*nconflicts = cl.n;
	free(edits);
	free(vals);
	prolly_diff_list_free(&dours);
	prolly_diff_list_free(&dtheirs);
}

void
vc_conflicts_free(VcConflict *c, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		free(c[i].table);
		free(c[i].key);
	}
	free(c);
}

/* -------------------------------------------------------------- commits */

bool
vc_commit_load(ProllyCtx *ctx, const PHash *h, VcCommit *out)
{
	size_t		len;
	const uint8_t *d = cs_get(ctx->cs, h, &len);
	const uint8_t *p,
			   *end;
	uint64_t	np,
				ml;

	if (d == NULL || len < 2 + PHASH_LEN || d[0] != CHUNK_TAG_COMMIT ||
		d[1] != CHUNK_FORMAT_VERSION)
		return false;
	p = d + 2;
	end = d + len;
	out->hash = *h;
	memcpy(out->root.b, p, PHASH_LEN);
	p += PHASH_LEN;
	if (!read_uvarint(&p, end, &out->generation) ||
		!read_uvarint(&p, end, &np) || np > VC_MAX_PARENTS)
		return false;
	out->nparents = (uint32_t) np;
	for (uint32_t i = 0; i < out->nparents; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			return false;
		memcpy(out->parents[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_uvarint(&p, end, &out->parent_gen[i]))
			return false;
	}
	if (!read_uvarint(&p, end, &out->time) || !read_uvarint(&p, end, &ml) ||
		ml != (uint64_t) (end - p))
		return false;
	out->msg = (const char *) p;
	out->msglen = ml;
	return true;
}

/* For commits that must exist: the parents of a commit being written. */
static VcCommit
must_load_commit(ProllyCtx *ctx, const PHash *h)
{
	VcCommit	c;

	if (!vc_commit_load(ctx, h, &c))
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(h, hex);
		fprintf(stderr, "vc: bad or missing commit %s\n", hex);
		abort();
	}
	return c;
}

PHash
vc_commit_write(ProllyCtx *ctx, const PHash *root, const PHash *parents,
				uint32_t nparents, uint64_t time, const char *msg)
{
	Buf			b = BUF_INIT;
	uint64_t	gens[VC_MAX_PARENTS];
	uint64_t	gen = 0;
	PHash		h;

	if (nparents > VC_MAX_PARENTS)
		abort();
	for (uint32_t i = 0; i < nparents; i++)
	{
		gens[i] = must_load_commit(ctx, &parents[i]).generation;
		if (gens[i] > gen)
			gen = gens[i];
	}
	buf_append_byte(&b, CHUNK_TAG_COMMIT);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, root->b, PHASH_LEN);
	buf_append_uvarint(&b, gen + 1);
	buf_append_uvarint(&b, nparents);
	for (uint32_t i = 0; i < nparents; i++)
	{
		buf_append(&b, parents[i].b, PHASH_LEN);
		buf_append_uvarint(&b, gens[i]);
	}
	buf_append_uvarint(&b, time);
	buf_append_uvarint(&b, strlen(msg));
	buf_append(&b, msg, strlen(msg));
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

/* Max-heap of commits ordered by generation. */
typedef struct GenEnt
{
	uint64_t	gen;
	PHash		h;
} GenEnt;

typedef struct GenHeap
{
	GenEnt	   *v;
	size_t		n;
	size_t		cap;
} GenHeap;

static void
heap_push(GenHeap *hp, uint64_t gen, const PHash *h)
{
	size_t		i;

	px_grow((void **) &hp->v, &hp->cap, hp->n + 1, sizeof(GenEnt));
	i = hp->n++;
	while (i > 0 && hp->v[(i - 1) / 2].gen < gen)
	{
		hp->v[i] = hp->v[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	hp->v[i].gen = gen;
	hp->v[i].h = *h;
}

static GenEnt
heap_pop(GenHeap *hp)
{
	GenEnt		top = hp->v[0];
	GenEnt		last = hp->v[--hp->n];
	size_t		i = 0;

	for (;;)
	{
		size_t		c = 2 * i + 1;

		if (c >= hp->n)
			break;
		if (c + 1 < hp->n && hp->v[c + 1].gen > hp->v[c].gen)
			c++;
		if (hp->v[c].gen <= last.gen)
			break;
		hp->v[i] = hp->v[c];
		i = c;
	}
	if (hp->n > 0)
		hp->v[i] = last;
	return top;
}

#define MB_A 1
#define MB_B 2
#define MB_DONE 4

/* Loads a commit that may lie beyond a cut; an absent one is VC_ERR_SEVERED. */
static int
load_commit(ProllyCtx *ctx, const PHash *h, VcCommit *out)
{
	if (vc_commit_load(ctx, h, out))
		return VC_OK;
	ctx->err_hash = *h;
	ctx->err = cs_has(ctx->cs, h) ? PROLLY_ERR_CORRUPT : VC_ERR_SEVERED;
	return ctx->err;
}

/*
 * Walks both histories newest generation first.  A commit's flags are final
 * when it is popped, because every descendant has a larger generation and
 * was popped before it, so the first commit reached from both sides is a
 * best common ancestor.  Cost is the number of commits newer than it.  The
 * answer itself is never loaded, so it may be a commit beyond a cut.  When
 * gate is set, every commit read must pass it.
 */
typedef int (*CommitGate) (void *arg, const PHash *h);

static int
merge_base_core(ProllyCtx *ctx, const PHash *a, const PHash *b,
				CommitGate gate, void *garg, PHash *out)
{
	PHashMap	flags;
	GenHeap		hp = {0};
	bool		created;
	int			result = 0;
	VcCommit	ca,
				cb;
	int			rc;

	if (phash_eq(a, b))
	{
		*out = *a;
		return 1;
	}
	if ((gate && ((rc = gate(garg, a)) != VC_OK || (rc = gate(garg, b)) != VC_OK)) ||
		(rc = load_commit(ctx, a, &ca)) != VC_OK ||
		(rc = load_commit(ctx, b, &cb)) != VC_OK)
		return rc;
	phmap_init(&flags);
	*phmap_ref(&flags, a, &created) |= MB_A;
	*phmap_ref(&flags, b, &created) |= MB_B;
	heap_push(&hp, ca.generation, a);
	heap_push(&hp, cb.generation, b);
	while (hp.n > 0)
	{
		GenEnt		e = heap_pop(&hp);
		uint64_t   *f = phmap_find(&flags, &e.h);
		uint64_t	mine;
		VcCommit	c;

		if (*f & MB_DONE)
			continue;
		*f |= MB_DONE;
		mine = *f & (MB_A | MB_B);
		if (mine == (MB_A | MB_B))
		{
			*out = e.h;
			result = 1;
			break;
		}
		if ((gate && (rc = gate(garg, &e.h)) != VC_OK) ||
			(rc = load_commit(ctx, &e.h, &c)) != VC_OK)
		{
			result = rc;
			break;
		}
		for (uint32_t i = 0; i < c.nparents; i++)
		{
			uint64_t   *pf = phmap_ref(&flags, &c.parents[i], &created);

			if ((*pf & mine) != mine)
			{
				*pf |= mine;
				heap_push(&hp, c.parent_gen[i], &c.parents[i]);
			}
		}
	}
	free(hp.v);
	phmap_free(&flags);
	return result;
}

int
vc_merge_base(ProllyCtx *ctx, const PHash *a, const PHash *b, PHash *out)
{
	return merge_base_core(ctx, a, b, NULL, NULL, out);
}

/* ----------------------------------------------------------------- repo */

/*
 * Where each known commit is, in r->loc: held by the window's history,
 * a member of segment n (stored as n + LOC_SEG_BASE), or a boundary: a
 * commit named as a parent whose content this repo never had.
 */
#define LOC_WINDOW 1
#define LOC_SEG_BASE 2
#define LOC_BOUNDARY UINT64_MAX

static int	reach_commit(VcRepo *r, const PHash *h, unsigned flags, VcCommit *c);
static int	window_add(VcRepo *r, PHashMap *win, const PHash *head, uint64_t depth);

static PHash
name_key(const char *name)
{
	PHash		k;

	phash_compute(name, strlen(name), &k);
	return k;
}

static void
pending_push(VcRepo *r, const PHash *h)
{
	px_grow((void **) &r->pending, &r->pendcap, r->npending + 1, sizeof(PHash));
	r->pending[r->npending++] = *h;
}

/* Adds (+1) or removes (-1) a head's window from the cover counts. */
static void
cover_head(VcRepo *r, const PHash *head, int delta)
{
	PHashMap	win;

	if (r->track_depth == 0)
		return;
	phmap_init(&win);
	window_add(r, &win, head, r->track_depth);
	for (size_t i = 0; i < win.cap; i++)
	{
		bool		created;
		uint64_t   *c;

		if (!win.used[i])
			continue;
		c = phmap_ref(&r->cover, &win.keys[i], &created);
		if (delta > 0)
			(*c)++;
		else if (--(*c) == 0)
			pending_push(r, &win.keys[i]);
	}
	phmap_free(&win);
}

/* A head moved from old to new; either may be NULL. */
static void
move_head(VcRepo *r, const PHash *old, const PHash *new)
{
	if (new)
		cover_head(r, new, +1);	/* add first, so nothing passes through 0 */
	if (old)
		cover_head(r, old, -1);
}

static void
set_working(VcRepo *r, VcBranch *b, const PHash *rv)
{
	PHash		old = b->working;

	cs_ref_root(r->ctx.cs, rv);
	b->working = *rv;
	cs_unref_root(r->ctx.cs, &old);
	prolly_release_pins(&r->ctx);
}

static VcBranch *
add_branch(VcRepo *r, const char *name, const PHash *head, const PHash *root)
{
	VcBranch   *b;

	px_grow((void **) &r->br, &r->cap, r->nbr + 1, sizeof(VcBranch));
	b = &r->br[r->nbr++];
	memset(b, 0, sizeof(*b));
	b->name = px_memdup(name, strlen(name) + 1);
	b->head = *head;
	b->head_root = *root;
	b->working = *root;
	cs_ref_root(r->ctx.cs, root);
	{
		bool		created;
		PHash		k = name_key(name);

		*phmap_ref(&r->brmap, &k, &created) = (uint64_t) (b - r->br);
	}
	move_head(r, NULL, head);
	return b;
}

static void
set_loc(VcRepo *r, const PHash *h, uint64_t where)
{
	bool		created;

	*phmap_ref(&r->loc, h, &created) = where;
}

static bool
in_window(VcRepo *r, const PHash *h)
{
	uint64_t   *v = phmap_find(&r->loc, h);

	return v && *v == LOC_WINDOW;
}

/* A commit enters the window's history; sever decides if it stays. */
static void
enter_window(VcRepo *r, const PHash *h)
{
	set_loc(r, h, LOC_WINDOW);
	if (r->track_depth)
		pending_push(r, h);
}

/* Writes a commit and keeps it in the window's history. */
static PHash
keep_commit(VcRepo *r, const PHash *root, const PHash *parents, uint32_t np,
			const char *msg)
{
	PHash		h = vc_commit_write(&r->ctx, root, parents, np, r->clock++, msg);

	cs_ref_root(r->ctx.cs, &h);
	enter_window(r, &h);
	return h;
}

static void
repo_init_empty(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	memset(r, 0, sizeof(*r));
	prolly_ctx_init(&r->ctx, cs, policy);
	phmap_init(&r->loc);
	phmap_init(&r->hint);
	phmap_init(&r->brmap);
	phmap_init(&r->imgmap);
	phmap_init(&r->cover);
	r->name = px_memdup("", 1);
}

/*
 * Repos started separately must not share history by accident, so each
 * init commit is unique: the repo clock starts from the time and a
 * process-wide counter.
 */
static uint64_t
unique_clock(void)
{
	static uint64_t counter;

	return ((uint64_t) time(NULL) << 24) + (++counter << 4);
}

void
vc_repo_init(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	PHash		root,
				commit;

	repo_init_empty(r, cs, policy);
	r->clock = unique_clock();
	root = vc_root_empty(&r->ctx);
	commit = keep_commit(r, &root, NULL, 0, "init");
	add_branch(r, "main", &commit, &root);
	prolly_release_pins(&r->ctx);
}

void
vc_repo_free(VcRepo *r)
{
	for (size_t i = 0; i < r->nbr; i++)
		free(r->br[i].name);
	free(r->br);
	for (size_t i = 0; i < r->nsegs; i++)
	{
		free(r->segs[i].commits);
		free(r->segs[i].archive_name);
	}
	free(r->segs);
	phmap_free(&r->loc);
	phmap_free(&r->hint);
	for (size_t i = 0; i < r->nhints; i++)
		free(r->hints[i]);
	free(r->hints);
	free(r->img_commit);
	free(r->img_hash);
	phmap_free(&r->brmap);
	phmap_free(&r->imgmap);
	phmap_free(&r->cover);
	free(r->pending);
	free(r->name);
	prolly_ctx_free(&r->ctx);
	memset(r, 0, sizeof(*r));
}

VcBranch *
vc_branch_find(VcRepo *r, const char *name)
{
	PHash		k = name_key(name);
	uint64_t   *v = phmap_find(&r->brmap, &k);

	if (v == NULL || strcmp(r->br[*v].name, name) != 0)
		return NULL;
	return &r->br[*v];
}

int
vc_branch_delete(VcRepo *r, const char *name)
{
	VcBranch   *b = vc_branch_find(r, name);
	size_t		i,
				last;
	PHash		k = name_key(name);

	if (b == NULL || (size_t) (b - r->br) == r->cur)
		return VC_ERR;
	i = (size_t) (b - r->br);
	move_head(r, &b->head, NULL);
	if (b->merging)
		move_head(r, &b->merge_head, NULL);
	cs_unref_root(r->ctx.cs, &b->working);
	free(b->name);
	phmap_remove(&r->brmap, &k);
	last = --r->nbr;
	if (i != last)
	{
		PHash		lk = name_key(r->br[last].name);

		r->br[i] = r->br[last];
		*phmap_find(&r->brmap, &lk) = i;
		if (r->cur == last)
			r->cur = i;
	}
	return VC_OK;
}

VcBranch *
vc_head(VcRepo *r)
{
	return &r->br[r->cur];
}

PHash
vc_working(VcRepo *r)
{
	return vc_head(r)->working;
}

void
vc_set_working(VcRepo *r, const PHash *rv)
{
	set_working(r, vc_head(r), rv);
}

int
vc_branch_create(VcRepo *r, const char *name, const PHash *commit)
{
	VcBranch   *cur = vc_head(r);
	PHash		head = cur->head,
				root = cur->head_root;

	if (vc_branch_find(r, name))
		return VC_ERR;
	if (commit && !phash_eq(commit, &cur->head))
	{
		VcCommit	c;
		int			rc;

		/* a commit outside the window must be attached before use */
		if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
			return rc;
		head = *commit;
		root = c.root;
	}
	add_branch(r, name, &head, &root);
	return VC_OK;
}

int
vc_checkout(VcRepo *r, const char *name)
{
	VcBranch   *b = vc_branch_find(r, name);

	if (b == NULL)
		return VC_ERR;
	r->cur = (size_t) (b - r->br);
	return VC_OK;
}

PHash
vc_commit_root(VcRepo *r, const PHash *root, const char *msg)
{
	VcBranch   *b = vc_head(r);
	PHash		parents[2] = {b->head, b->merge_head};
	PHash		h = keep_commit(r, root, parents, b->merging ? 2 : 1, msg);

	b = vc_head(r);
	move_head(r, &parents[0], &h);
	if (b->merging)
		move_head(r, &parents[1], NULL);
	b->head = h;
	b->head_root = *root;
	b->merging = false;
	prolly_release_pins(&r->ctx);
	return h;
}

PHash
vc_commit(VcRepo *r, const char *msg)
{
	PHash		root = vc_head(r)->working;

	return vc_commit_root(r, &root, msg);
}

int
vc_reset_hard(VcRepo *r, const PHash *commit)
{
	VcBranch   *b = vc_head(r);
	VcCommit	c;
	int			rc;

	if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
		return rc;
	move_head(r, &b->head, commit);
	if (b->merging)
		move_head(r, &b->merge_head, NULL);
	b->head = *commit;
	b->head_root = c.root;
	b->merging = false;
	set_working(r, b, &c.root);
	return VC_OK;
}

int
vc_merge(VcRepo *r, const char *branch, VcMergeResult *res)
{
	VcBranch   *b = vc_head(r);
	VcBranch   *other = vc_branch_find(r, branch);
	PHash		ours,
				theirs,
				base,
				merged;
	VcCommit	cbase,
				ctheirs;
	int			rc;

	memset(res, 0, sizeof(*res));
	if (other == NULL || b->merging || !phash_eq(&b->working, &b->head_root))
		return VC_ERR;			/* unknown branch, or uncommitted changes */
	ours = b->head;
	theirs = other->head;
	rc = vc_merge_base(&r->ctx, &ours, &theirs, &base);
	if (rc != 1)
		return rc < 0 ? rc : VC_ERR;
	if (phash_eq(&base, &theirs))
	{
		res->status = VC_MERGE_UP_TO_DATE;
		res->commit = ours;
		return VC_OK;
	}
	if ((rc = load_commit(&r->ctx, &theirs, &ctheirs)) != VC_OK)
		return rc;
	if (phash_eq(&base, &ours))
	{
		move_head(r, &b->head, &theirs);
		b->head = theirs;
		b->head_root = ctheirs.root;
		set_working(r, b, &ctheirs.root);
		res->status = VC_MERGE_FAST_FORWARD;
		res->commit = theirs;
		return VC_OK;
	}
	/* the base may be beyond a cut even when both heads are not */
	if ((rc = load_commit(&r->ctx, &base, &cbase)) != VC_OK)
		return rc;
	vc_merge_roots(&r->ctx, &cbase.root, &b->head_root, &ctheirs.root, &merged,
				   &res->conflicts, &res->nconflicts);
	b->merging = true;
	b->merge_head = theirs;
	move_head(r, NULL, &theirs);
	set_working(r, b, &merged);
	if (res->nconflicts > 0)
	{
		res->status = VC_MERGE_CONFLICTS;
		return VC_OK;
	}
	res->commit = vc_commit(r, "merge");
	res->status = VC_MERGE_COMMITTED;
	return VC_OK;
}

void
vc_merge_result_free(VcMergeResult *res)
{
	vc_conflicts_free(res->conflicts, res->nconflicts);
	memset(res, 0, sizeof(*res));
}

/* With VC_WINDOW_ONLY, a commit outside the window is refused, present or not. */
static int
reach_commit(VcRepo *r, const PHash *h, unsigned flags, VcCommit *c)
{
	if ((flags & VC_WINDOW_ONLY) && !in_window(r, h))
	{
		r->ctx.err_hash = *h;
		r->ctx.err = VC_ERR_OUTSIDE_WINDOW;
		return VC_ERR_OUTSIDE_WINDOW;
	}
	return load_commit(&r->ctx, h, c);
}

static int
window_gate(void *arg, const PHash *h)
{
	VcRepo	   *r = arg;

	if (in_window(r, h))
		return VC_OK;
	r->ctx.err_hash = *h;
	r->ctx.err = VC_ERR_OUTSIDE_WINDOW;
	return VC_ERR_OUTSIDE_WINDOW;
}

int
vc_merge_base_in(VcRepo *r, const PHash *a, const PHash *b, unsigned flags,
				 PHash *out)
{
	bool		only = (flags & VC_WINDOW_ONLY) != 0;
	int			rc = merge_base_core(&r->ctx, a, b, only ? window_gate : NULL,
									 r, out);

	/*
	 * The walk may name a base past the edge from parent links alone; in
	 * window-only mode that is still outside, with the base in err_hash.
	 */
	if (rc == 1 && only)
		rc = window_gate(r, out);
	return rc == VC_OK ? 1 : rc;
}

int
vc_diff_commits(VcRepo *r, const PHash *from, const PHash *to, unsigned flags,
				VcTableChangeFn fn, void *arg)
{
	VcCommit	a,
				b;
	int			rc;

	if ((rc = reach_commit(r, from, flags, &a)) != VC_OK ||
		(rc = reach_commit(r, to, flags, &b)) != VC_OK)
		return rc;
	vc_diff_tables(&r->ctx, &a.root, &b.root, fn, arg);
	return VC_OK;
}

int
vc_log(VcRepo *r, const PHash *start, size_t max, unsigned flags,
	   VcLogFn fn, void *arg)
{
	PHash		h = *start;

	for (size_t i = 0; i < max; i++)
	{
		VcCommit	c;
		int			rc = reach_commit(r, &h, flags, &c);

		if (rc != VC_OK)
			return rc;
		fn(arg, &c);
		if (c.nparents == 0)
			break;
		h = c.parents[0];
	}
	return VC_OK;
}

/* ------------------------------------------------------- saving and opening */

static void
put_hash(Buf *b, const PHash *h)
{
	buf_append(b, h->b, PHASH_LEN);
}

static void
put_str(Buf *b, const char *s)
{
	size_t		n = s ? strlen(s) : 0;

	buf_append_uvarint(b, n);
	buf_append(b, s, n);
}

PHash
vc_repo_save(VcRepo *r)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_REPO);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	put_str(&b, r->name);
	buf_append_uvarint(&b, r->clock);
	buf_append_uvarint(&b, r->window_depth);
	buf_append_uvarint(&b, r->track_depth);
	buf_append_uvarint(&b, r->cur);
	buf_append_uvarint(&b, r->nbr);
	for (size_t i = 0; i < r->nbr; i++)
	{
		VcBranch   *br = &r->br[i];

		put_str(&b, br->name);
		put_hash(&b, &br->head);
		put_hash(&b, &br->head_root);
		put_hash(&b, &br->working);
		buf_append_byte(&b, br->merging);
		put_hash(&b, &br->merge_head);
	}
	buf_append_uvarint(&b, r->loc.n);
	for (size_t i = 0; i < r->loc.cap; i++)
		if (r->loc.used[i])
		{
			put_hash(&b, &r->loc.keys[i]);
			buf_append_uvarint(&b, r->loc.vals[i]);
		}
	buf_append_uvarint(&b, r->nsegs);
	for (size_t i = 0; i < r->nsegs; i++)
	{
		VcSegment  *sg = &r->segs[i];

		buf_append_byte(&b, (uint8_t) sg->state);
		buf_append_uvarint(&b, sg->n);
		for (size_t j = 0; j < sg->n; j++)
			put_hash(&b, &sg->commits[j]);
		put_str(&b, sg->archive_name);
		put_hash(&b, &sg->manifest);
	}
	buf_append_uvarint(&b, r->nhints);
	for (size_t i = 0; i < r->nhints; i++)
		put_str(&b, r->hints[i]);
	buf_append_uvarint(&b, r->hint.n);
	for (size_t i = 0; i < r->hint.cap; i++)
		if (r->hint.used[i])
		{
			put_hash(&b, &r->hint.keys[i]);
			buf_append_uvarint(&b, r->hint.vals[i]);
		}
	buf_append_uvarint(&b, r->nimg);
	for (size_t i = 0; i < r->nimg; i++)
	{
		put_hash(&b, &r->img_commit[i]);
		put_hash(&b, &r->img_hash[i]);
	}
	vc_must(&r->ctx, prolly_put_chunk(&r->ctx, b.data, b.len, &h));
	buf_free(&b);
	if (!r->has_state || !phash_eq(&r->state, &h))
	{
		cs_ref_root(r->ctx.cs, &h);
		if (r->has_state)
			cs_unref_root(r->ctx.cs, &r->state);
	}
	r->state = h;
	r->has_state = true;
	prolly_release_pins(&r->ctx);
	return h;
}

typedef struct Reader
{
	const uint8_t *p,
			   *end;
	bool		ok;
} Reader;

static uint64_t
rd_uv(Reader *rd)
{
	uint64_t	v = 0;

	if (rd->ok && !read_uvarint(&rd->p, rd->end, &v))
		rd->ok = false;
	return v;
}

static PHash
rd_hash(Reader *rd)
{
	PHash		h = {{0}};

	if (rd->ok && rd->end - rd->p >= PHASH_LEN)
	{
		memcpy(h.b, rd->p, PHASH_LEN);
		rd->p += PHASH_LEN;
	}
	else
		rd->ok = false;
	return h;
}

static char *
rd_str(Reader *rd)
{
	uint64_t	n = rd_uv(rd);
	char	   *s;

	if (!rd->ok || n > (uint64_t) (rd->end - rd->p))
	{
		rd->ok = false;
		return px_memdup("", 1);
	}
	s = px_malloc(n + 1);
	memcpy(s, rd->p, n);
	s[n] = '\0';
	rd->p += n;
	return s;
}

int
vc_repo_open(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy, const PHash *state)
{
	size_t		len;
	const uint8_t *d = cs_get(cs, state, &len);
	Reader		rd;
	uint64_t	n,
				track;

	repo_init_empty(r, cs, policy);
	if (d == NULL || len < 2 || d[0] != CHUNK_TAG_REPO || d[1] != CHUNK_FORMAT_VERSION)
		return VC_ERR;
	rd = (Reader) {d + 2, d + len, true};
	free(r->name);
	r->name = rd_str(&rd);
	r->clock = rd_uv(&rd);
	r->window_depth = (uint32_t) rd_uv(&rd);
	track = rd_uv(&rd);
	r->cur = rd_uv(&rd);
	n = rd_uv(&rd);
	for (uint64_t i = 0; i < n && rd.ok; i++)
	{
		VcBranch   *b;
		bool		created;
		PHash		k;

		px_grow((void **) &r->br, &r->cap, r->nbr + 1, sizeof(VcBranch));
		b = &r->br[r->nbr];
		memset(b, 0, sizeof(*b));
		b->name = rd_str(&rd);
		b->head = rd_hash(&rd);
		b->head_root = rd_hash(&rd);
		b->working = rd_hash(&rd);
		b->merging = rd.ok && rd.p < rd.end && *rd.p++ != 0;
		b->merge_head = rd_hash(&rd);
		k = name_key(b->name);
		*phmap_ref(&r->brmap, &k, &created) = r->nbr;
		r->nbr++;
	}
	n = rd_uv(&rd);
	for (uint64_t i = 0; i < n && rd.ok; i++)
	{
		PHash		h = rd_hash(&rd);

		set_loc(r, &h, rd_uv(&rd));
	}
	n = rd_uv(&rd);
	for (uint64_t i = 0; i < n && rd.ok; i++)
	{
		VcSegment  *sg;

		px_grow((void **) &r->segs, &r->segcap, r->nsegs + 1, sizeof(VcSegment));
		sg = &r->segs[r->nsegs++];
		memset(sg, 0, sizeof(*sg));
		sg->state = (VcSegState) (rd.ok && rd.p < rd.end ? *rd.p++ : 0);
		sg->n = rd_uv(&rd);
		if (sg->n > len)
			rd.ok = false;
		sg->commits = px_malloc((rd.ok ? sg->n : 0) * sizeof(PHash) + 1);
		for (size_t j = 0; j < sg->n && rd.ok; j++)
			sg->commits[j] = rd_hash(&rd);
		sg->archive_name = rd_str(&rd);
		sg->manifest = rd_hash(&rd);
	}
	n = rd_uv(&rd);
	for (uint64_t i = 0; i < n && rd.ok; i++)
	{
		px_grow((void **) &r->hints, &r->hintcap, r->nhints + 1, sizeof(char *));
		r->hints[r->nhints++] = rd_str(&rd);
	}
	n = rd_uv(&rd);
	for (uint64_t i = 0; i < n && rd.ok; i++)
	{
		PHash		h = rd_hash(&rd);
		bool		created;

		*phmap_ref(&r->hint, &h, &created) = rd_uv(&rd);
	}
	n = rd_uv(&rd);
	for (uint64_t i = 0; i < n && rd.ok; i++)
	{
		bool		created;

		px_grow((void **) &r->img_commit, &r->imgcap, r->nimg + 1, sizeof(PHash));
		r->img_hash = px_realloc(r->img_hash, r->imgcap * sizeof(PHash));
		r->img_commit[r->nimg] = rd_hash(&rd);
		r->img_hash[r->nimg] = rd_hash(&rd);
		*phmap_ref(&r->imgmap, &r->img_commit[r->nimg], &created) = r->nimg;
		r->nimg++;
	}
	if (!rd.ok || rd.p != rd.end || r->nbr == 0 || r->cur >= r->nbr)
		return VC_ERR;
	r->state = *state;
	r->has_state = true;
	/* holds are already in place; the window's counts are derived */
	if (track)
		vc_window_track(r, (uint32_t) track);
	return VC_OK;
}

void
vc_segment_set_archive(VcRepo *r, uint32_t seg, ChunkStore *archive)
{
	if (seg < r->nsegs)
		r->segs[seg].archive = archive;
}

/* ---------------------------------------------------------- commit images */

static ssize_t
image_slot(VcRepo *r, const PHash *commit)
{
	uint64_t   *v = phmap_find(&r->imgmap, commit);

	return v ? (ssize_t) *v : -1;
}

static void
image_release(VcRepo *r, const PHash *commit)
{
	ssize_t		i = image_slot(r, commit);

	if (i < 0)
		return;
	cs_unref_root(r->ctx.cs, &r->img_hash[i]);
	phmap_remove(&r->imgmap, commit);
	r->nimg--;
	if ((size_t) i != r->nimg)
	{
		r->img_commit[i] = r->img_commit[r->nimg];
		r->img_hash[i] = r->img_hash[r->nimg];
		*phmap_find(&r->imgmap, &r->img_commit[i]) = (uint64_t) i;
	}
}

int
vc_image_set(VcRepo *r, const PHash *commit, const PHash *image)
{
	ssize_t		i;

	if (!in_window(r, commit))
	{
		r->ctx.err_hash = *commit;
		return VC_ERR_OUTSIDE_WINDOW;
	}
	if (!cs_has(r->ctx.cs, image))
	{
		r->ctx.err_hash = *image;
		return VC_ERR;
	}
	cs_ref_root(r->ctx.cs, image);
	i = image_slot(r, commit);
	if (i >= 0)
	{
		cs_unref_root(r->ctx.cs, &r->img_hash[i]);
		r->img_hash[i] = *image;
	}
	else
	{
		px_grow((void **) &r->img_commit, &r->imgcap, r->nimg + 1, sizeof(PHash));
		r->img_hash = px_realloc(r->img_hash, r->imgcap * sizeof(PHash));
		r->img_commit[r->nimg] = *commit;
		r->img_hash[r->nimg] = *image;
		{
			bool		created;

			*phmap_ref(&r->imgmap, commit, &created) = r->nimg;
		}
		r->nimg++;
	}
	prolly_release_pins(&r->ctx);
	return VC_OK;
}

bool
vc_image_get(VcRepo *r, const PHash *commit, PHash *image)
{
	ssize_t		i = image_slot(r, commit);

	if (i < 0)
		return false;
	*image = r->img_hash[i];
	return true;
}

/* --------------------------------------------------------------- window */

/*
 * Adds to win every commit within depth generations of head: those with
 * generation > gen(head) - depth on some path from it.  win maps each
 * commit to 1 + the lowest generation bound it was expanded under, so a
 * commit reached again under a lower bound is expanded again.  Parent
 * generations live in the child, so nothing outside the window is read.
 */
static int
window_add(VcRepo *r, PHashMap *win, const PHash *head, uint64_t depth)
{
	VcCommit	c;
	PHash	   *stack = NULL;
	size_t		n = 0,
				cap = 0;
	uint64_t	bound;
	int			rc = load_commit(&r->ctx, head, &c);

	if (rc != VC_OK)
		return rc;
	bound = c.generation > depth ? c.generation - depth : 0;
	px_grow((void **) &stack, &cap, 1, sizeof(PHash));
	stack[n++] = *head;
	while (n > 0 && rc == VC_OK)
	{
		PHash		h = stack[--n];
		bool		created;
		uint64_t   *v = phmap_ref(win, &h, &created);

		if (!created && *v <= bound + 1)
			continue;
		*v = bound + 1;
		/* a wider window can reach commits already cut; they stay cut */
		if (!vc_commit_load(&r->ctx, &h, &c))
			continue;
		for (uint32_t i = 0; i < c.nparents; i++)
			if (c.parent_gen[i] > bound)
			{
				px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
				stack[n++] = c.parents[i];
			}
	}
	free(stack);
	return rc;
}

int
vc_sever(VcRepo *r, const VcWindow *w, uint32_t *seg_out)
{
	PHashMap	win;
	PHash	   *members = NULL;
	size_t		n = 0,
				cap = 0;
	int			rc = VC_OK;
	VcSegment  *seg;

	*seg_out = VC_NO_SEGMENT;
	if (w->depth == 0)
		return VC_ERR;			/* branch heads are always in the window */
	if (r->track_depth == w->depth && w->npinned == 0)
	{
		/* incremental: only commits whose cover fell to zero can leave */
		PHashMap	seen;
		bool		created;

		phmap_init(&seen);
		for (size_t i = 0; i < r->npending; i++)
		{
			uint64_t   *c = phmap_find(&r->cover, &r->pending[i]);

			phmap_ref(&seen, &r->pending[i], &created);
			if (created && in_window(r, &r->pending[i]) && (c == NULL || *c == 0))
			{
				px_grow((void **) &members, &cap, n + 1, sizeof(PHash));
				members[n++] = r->pending[i];
			}
		}
		phmap_free(&seen);
		r->npending = 0;
		goto detach;
	}
	phmap_init(&win);
	for (size_t i = 0; i < r->nbr && rc == VC_OK; i++)
	{
		rc = window_add(r, &win, &r->br[i].head, w->depth);
		if (rc == VC_OK && r->br[i].merging)
			rc = window_add(r, &win, &r->br[i].merge_head, w->depth);
	}
	for (size_t i = 0; i < w->npinned && rc == VC_OK; i++)
		rc = window_add(r, &win, &w->pinned[i], 1);
	if (rc != VC_OK)
	{
		phmap_free(&win);
		return rc;
	}

	/* everything the window's history holds that the window no longer covers */
	for (size_t i = 0; i < r->loc.cap; i++)
		if (r->loc.used[i] && r->loc.vals[i] == LOC_WINDOW &&
			!phmap_find(&win, &r->loc.keys[i]))
		{
			px_grow((void **) &members, &cap, n + 1, sizeof(PHash));
			members[n++] = r->loc.keys[i];
		}
	phmap_free(&win);
detach:
	if (n == 0)
	{
		free(members);
		return VC_OK;
	}

	/* commit images are only for the window: severed commits let theirs go */
	for (size_t i = 0; i < n; i++)
		image_release(r, &members[i]);

	/* detach: the holds move to the segment; no chunk is read, copied or freed */
	px_grow((void **) &r->segs, &r->segcap, r->nsegs + 1, sizeof(VcSegment));
	seg = &r->segs[r->nsegs];
	memset(seg, 0, sizeof(*seg));
	seg->state = VC_SEG_LOCAL;
	seg->commits = members;
	seg->n = n;
	for (size_t i = 0; i < n; i++)
		*phmap_find(&r->loc, &members[i]) = LOC_SEG_BASE + r->nsegs;
	*seg_out = (uint32_t) r->nsegs++;
	return VC_OK;
}

int
vc_window_track(VcRepo *r, uint32_t depth)
{
	phmap_free(&r->cover);
	phmap_init(&r->cover);
	r->npending = 0;
	r->track_depth = depth;
	if (depth == 0)
		return VC_OK;
	for (size_t i = 0; i < r->nbr; i++)
	{
		cover_head(r, &r->br[i].head, +1);
		if (r->br[i].merging)
			cover_head(r, &r->br[i].merge_head, +1);
	}
	/* everything already in the window is a candidate once */
	for (size_t i = 0; i < r->loc.cap; i++)
		if (r->loc.used[i] && r->loc.vals[i] == LOC_WINDOW)
			pending_push(r, &r->loc.keys[i]);
	return VC_OK;
}

bool
vc_window_verify(VcRepo *r)
{
	PHashMap	want,
				pend;
	bool		ok = true,
				created;

	if (r->track_depth == 0)
		return true;
	phmap_init(&want);
	for (size_t i = 0; i < r->nbr; i++)
	{
		PHashMap	win;

		phmap_init(&win);
		window_add(r, &win, &r->br[i].head, r->track_depth);
		if (r->br[i].merging)
		{
			for (size_t j = 0; j < win.cap; j++)
				if (win.used[j])
					(*phmap_ref(&want, &win.keys[j], &created))++;
			phmap_free(&win);
			phmap_init(&win);
			window_add(r, &win, &r->br[i].merge_head, r->track_depth);
		}
		for (size_t j = 0; j < win.cap; j++)
			if (win.used[j])
				(*phmap_ref(&want, &win.keys[j], &created))++;
		phmap_free(&win);
	}
	/* counts agree, in both directions */
	for (size_t i = 0; i < want.cap && ok; i++)
		if (want.used[i])
		{
			uint64_t   *c = phmap_find(&r->cover, &want.keys[i]);

			ok = c && *c == want.vals[i];
		}
	for (size_t i = 0; i < r->cover.cap && ok; i++)
		if (r->cover.used[i] && r->cover.vals[i] > 0)
		{
			uint64_t   *c = phmap_find(&want, &r->cover.keys[i]);

			ok = c && *c == r->cover.vals[i];
		}
	/* every uncovered commit in the window is waiting to be severed */
	phmap_init(&pend);
	for (size_t i = 0; i < r->npending; i++)
		phmap_ref(&pend, &r->pending[i], &created);
	for (size_t i = 0; i < r->loc.cap && ok; i++)
		if (r->loc.used[i] && r->loc.vals[i] == LOC_WINDOW &&
			!phmap_find(&want, &r->loc.keys[i]))
			ok = phmap_find(&pend, &r->loc.keys[i]) != NULL;
	phmap_free(&pend);
	phmap_free(&want);
	return ok;
}

/* Commits of one segment that are not members: its boundary edges. */
static void
segment_bounds(ProllyCtx *ctx, const PHash *members, size_t n, PHashMap *mem,
			   PHash **out, size_t *nout)
{
	size_t		cap = 0;
	PHashMap	seen;
	bool		created;

	phmap_init(&seen);
	*out = NULL;
	*nout = 0;
	for (size_t i = 0; i < n; i++)
	{
		VcCommit	c;

		if (!vc_commit_load(ctx, &members[i], &c))
			continue;
		for (uint32_t j = 0; j < c.nparents; j++)
		{
			if (phmap_find(mem, &c.parents[j]))
				continue;
			phmap_ref(&seen, &c.parents[j], &created);
			if (!created)
				continue;
			px_grow((void **) out, &cap, *nout + 1, sizeof(PHash));
			(*out)[(*nout)++] = c.parents[j];
		}
	}
	phmap_free(&seen);
}

/*
 * Exports a segment: copies each member with its whole tree to the
 * archive, then writes a manifest there that names the members, their
 * generations, the segment's boundary and the source repo, and holds them.
 * Only then are the local holds released, which truncates local history
 * to the window.
 */
int
vc_segment_export(VcRepo *r, uint32_t id, const VcSource *archive)
{
	VcSegment  *seg;
	PinSet		ps;
	PHash		bad,
				manifest;
	PHashMap	mem;
	PHash	   *bounds;
	size_t		nb;
	Buf			b = BUF_INIT;
	bool		created;
	int			rc;

	if (id >= r->nsegs || archive->cs == r->ctx.cs)
		return VC_ERR;
	seg = &r->segs[id];
	if (seg->state != VC_SEG_LOCAL)
		return VC_ERR;
	pinset_init(&ps, archive->cs);
	for (size_t i = 0; i < seg->n; i++)
	{
		rc = cs_copy_closure(r->ctx.cs, &ps, &seg->commits[i], true, &bad);
		if (rc != CS_OK)
		{
			pinset_free(&ps);
			r->ctx.err_hash = bad;
			return rc;
		}
	}

	phmap_init(&mem);
	for (size_t i = 0; i < seg->n; i++)
		phmap_ref(&mem, &seg->commits[i], &created);
	segment_bounds(&r->ctx, seg->commits, seg->n, &mem, &bounds, &nb);
	phmap_free(&mem);
	buf_append_byte(&b, CHUNK_TAG_MANIFEST);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append_uvarint(&b, strlen(r->name));
	buf_append(&b, r->name, strlen(r->name));
	buf_append_uvarint(&b, seg->n);
	for (size_t i = 0; i < seg->n; i++)
	{
		VcCommit	c;

		vc_commit_load(&r->ctx, &seg->commits[i], &c);
		buf_append(&b, seg->commits[i].b, PHASH_LEN);
		buf_append_uvarint(&b, c.generation);
	}
	buf_append_uvarint(&b, nb);
	for (size_t i = 0; i < nb; i++)
		buf_append(&b, bounds[i].b, PHASH_LEN);
	free(bounds);
	rc = cs_put(archive->cs, b.data, b.len, &manifest, &bad);
	buf_free(&b);
	if (rc != CS_OK)
	{
		pinset_free(&ps);
		r->ctx.err_hash = bad;
		return rc;
	}
	cs_ref_root(archive->cs, &manifest);
	pinset_free(&ps);

	for (size_t i = 0; i < seg->n; i++)
		cs_unref_root(r->ctx.cs, &seg->commits[i]);
	seg->state = VC_SEG_EXPORTED;
	seg->archive = archive->cs;
	seg->archive_name = px_memdup(archive->name, strlen(archive->name) + 1);
	seg->manifest = manifest;
	return VC_OK;
}

/*
 * Discards a segment.  A local one is let go here; an exported one is let
 * go in its archive, which frees exactly what no other manifest there holds.
 */
int
vc_segment_discard(VcRepo *r, uint32_t id)
{
	VcSegment  *seg;

	if (id >= r->nsegs)
		return VC_ERR;
	seg = &r->segs[id];
	if (seg->state == VC_SEG_LOCAL)
		for (size_t i = 0; i < seg->n; i++)
			cs_unref_root(r->ctx.cs, &seg->commits[i]);
	else if (seg->state == VC_SEG_EXPORTED)
		cs_unref_root(seg->archive, &seg->manifest);
	else
		return VC_ERR;
	seg->state = VC_SEG_DISCARDED;
	seg->archive = NULL;
	return VC_OK;
}

int
vc_segment_attach(VcRepo *r, uint32_t id)
{
	VcSegment  *seg;

	if (id >= r->nsegs)
		return VC_ERR;
	seg = &r->segs[id];
	if (seg->state == VC_SEG_DISCARDED)
		return VC_ERR_SEVERED;
	if (seg->state == VC_SEG_EXPORTED)
	{
		PHash		bad;

		for (size_t i = 0; i < seg->n; i++)
		{
			int			rc = cs_copy_closure(seg->archive, &r->ctx.pins,
											 &seg->commits[i], true, &bad);

			if (rc != CS_OK)
			{
				prolly_release_pins(&r->ctx);
				r->ctx.err_hash = bad;
				return rc;
			}
		}
		for (size_t i = 0; i < seg->n; i++)
			cs_ref_root(r->ctx.cs, &seg->commits[i]);
		prolly_release_pins(&r->ctx);
	}
	else if (seg->state != VC_SEG_LOCAL)
		return VC_ERR;
	/* the holds move back to the window's history */
	for (size_t i = 0; i < seg->n; i++)
		enter_window(r, &seg->commits[i]);
	seg->state = VC_SEG_ATTACHED;
	seg->archive = NULL;
	return VC_OK;
}

VcLocation
vc_locate(VcRepo *r, const PHash *h, uint32_t *seg)
{
	uint64_t   *v = phmap_find(&r->loc, h);
	uint64_t	id;

	if (v == NULL)
		return VC_LOC_UNKNOWN;
	if (*v == LOC_WINDOW)
		return VC_LOC_WINDOW;
	if (*v == LOC_BOUNDARY)
		return VC_LOC_BOUNDARY;
	id = *v - LOC_SEG_BASE;
	if (seg)
		*seg = (uint32_t) id;
	switch (r->segs[id].state)
	{
		case VC_SEG_LOCAL:
			return VC_LOC_SEGMENT;
		case VC_SEG_EXPORTED:
			return VC_LOC_EXPORTED;
		case VC_SEG_DISCARDED:
			return VC_LOC_DISCARDED;
		default:
			return VC_LOC_UNKNOWN;
	}
}

/* ------------------------------------------------------------- archives */

typedef struct ManifestAcc
{
	ChunkStore *cs;
	PHash	   *v;
	size_t		n;
	size_t		cap;
} ManifestAcc;

static void
manifest_visit(void *arg, const PHash *h)
{
	ManifestAcc *acc = arg;
	size_t		len;
	const uint8_t *d = acc->cs->ops->get(acc->cs, h, &len);

	if (d && len >= 2 && d[0] == CHUNK_TAG_MANIFEST)
	{
		px_grow((void **) &acc->v, &acc->cap, acc->n + 1, sizeof(PHash));
		acc->v[acc->n++] = *h;
	}
}

void
vc_archive_list(ChunkStore *archive, PHash **manifests, size_t *n)
{
	ManifestAcc acc = {archive, NULL, 0, 0};

	/* a scan: archives are cold and listing them is rare */
	archive->ops->foreach(archive, manifest_visit, &acc);
	*manifests = acc.v;
	*n = acc.n;
}

bool
vc_manifest_load(ChunkStore *archive, const PHash *h, VcManifest *m)
{
	size_t		len;
	const uint8_t *d = cs_get(archive, h, &len);
	const uint8_t *p,
			   *end;
	uint64_t	nl,
				n,
				nb;

	memset(m, 0, sizeof(*m));
	if (d == NULL || len < 2 || d[0] != CHUNK_TAG_MANIFEST ||
		d[1] != CHUNK_FORMAT_VERSION)
		return false;
	p = d + 2;
	end = d + len;
	if (!read_uvarint(&p, end, &nl) || nl > (uint64_t) (end - p))
		return false;
	m->repo = px_malloc(nl + 1);
	memcpy(m->repo, p, nl);
	m->repo[nl] = '\0';
	p += nl;
	if (!read_uvarint(&p, end, &n) || n > len)
		goto bad;
	m->members = px_malloc(n * sizeof(PHash));
	m->gens = px_malloc(n * sizeof(uint64_t));
	for (uint64_t i = 0; i < n; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			goto bad;
		memcpy(m->members[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_uvarint(&p, end, &m->gens[i]))
			goto bad;
		m->n++;
	}
	if (!read_uvarint(&p, end, &nb) || nb > len)
		goto bad;
	m->bounds = px_malloc(nb * sizeof(PHash));
	for (uint64_t i = 0; i < nb; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			goto bad;
		memcpy(m->bounds[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		m->nb++;
	}
	if (p == end)
		return true;
bad:
	vc_manifest_free(m);
	return false;
}

void
vc_manifest_free(VcManifest *m)
{
	free(m->repo);
	free(m->members);
	free(m->gens);
	free(m->bounds);
	memset(m, 0, sizeof(*m));
}

/* ----------------------------------------------------------------- hints */

void
vc_repo_set_name(VcRepo *r, const char *name)
{
	free(r->name);
	r->name = px_memdup(name, strlen(name) + 1);
}

void
vc_set_window_depth(VcRepo *r, uint32_t depth)
{
	r->window_depth = depth;
}

static void
set_hint(VcRepo *r, const PHash *h, const char *where)
{
	size_t		i;
	bool		created;

	if (where == NULL || where[0] == '\0')
		return;
	for (i = 0; i < r->nhints; i++)
		if (strcmp(r->hints[i], where) == 0)
			break;
	if (i == r->nhints)
	{
		px_grow((void **) &r->hints, &r->hintcap, r->nhints + 1, sizeof(char *));
		r->hints[r->nhints++] = px_memdup(where, strlen(where) + 1);
	}
	*phmap_ref(&r->hint, h, &created) = i + 1;
}

/* Where this repo says a commit it lacks can be found. */
const char *
vc_hint(VcRepo *r, const PHash *h)
{
	uint32_t	seg;
	uint64_t   *v;

	if (vc_locate(r, h, &seg) == VC_LOC_EXPORTED)
		return r->segs[seg].archive_name;
	v = phmap_find(&r->hint, h);
	return v ? r->hints[*v - 1] : NULL;
}

static const char *
hint_from(VcRepo *src, const PHash *h)
{
	return vc_hint(src, h);
}

/* -------------------------------------------------------------- remotes */

static int
gen_cmp(const void *pa, const void *pb)
{
	const GenEnt *a = pa;
	const GenEnt *b = pb;

	return (a->gen > b->gen) - (a->gen < b->gen);
}

/* A planned transfer: commits to send, oldest first, and new boundaries. */
typedef struct Plan
{
	GenEnt	   *send;
	size_t		nsend;
	size_t		sendcap;
	PHash	   *bound;
	const char **bhint;
	size_t		nbound;
	size_t		boundcap;
	bool		connected;		/* reached a commit the receiver knows */
	bool		cut;			/* reached a commit the sender has cut */
	PHash		first_cut;
} Plan;

static void
plan_free(Plan *p)
{
	free(p->send);
	free(p->bound);
	free(p->bhint);
	memset(p, 0, sizeof(*p));
}

static void
plan_bound(Plan *p, const PHash *h, const char *hint)
{
	px_grow((void **) &p->bound, &p->boundcap, p->nbound + 1, sizeof(PHash));
	p->bhint = px_realloc(p->bhint, p->boundcap * sizeof(char *));
	p->bound[p->nbound] = *h;
	p->bhint[p->nbound] = hint;
	p->nbound++;
}

/*
 * Plans what src must send so dst has heads.  It walks parents from each
 * head, stopping at any commit dst already knows, whatever its location
 * there: discarded or exported history is never sent back implicitly.
 * With depth > 0, a path stops depth generations below its head; what lies
 * past becomes a boundary.  A parent src itself lacks is a boundary too,
 * hinting where src says it lives.
 */
static void
plan_walk(VcRepo *src, VcRepo *dst, const PHash *heads, size_t nheads,
		  uint32_t depth, Plan *p)
{
	PHashMap	seen,
				planned;
	PHash	   *stack = NULL;
	uint64_t   *sbound = NULL;
	size_t		n = 0,
				cap = 0;
	bool		created;

	phmap_init(&seen);
	phmap_init(&planned);
	for (size_t i = 0; i < nheads; i++)
	{
		VcCommit	c;
		uint64_t	b = 0;

		if (depth > 0 && vc_commit_load(&src->ctx, &heads[i], &c))
			b = c.generation > depth ? c.generation - depth : 0;
		px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
		sbound = px_realloc(sbound, cap * sizeof(uint64_t));
		stack[n] = heads[i];
		sbound[n++] = b;
	}
	while (n > 0)
	{
		PHash		h = stack[--n];
		uint64_t	b = sbound[n];
		uint64_t   *v;
		VcCommit	c;

		if (phmap_find(&dst->loc, &h))
		{
			p->connected = true;
			continue;
		}
		v = phmap_ref(&seen, &h, &created);
		if (!created && *v <= b + 1)
			continue;
		*v = b + 1;
		if (!vc_commit_load(&src->ctx, &h, &c))
		{
			if (!p->cut)
				p->first_cut = h;
			p->cut = true;
			plan_bound(p, &h, hint_from(src, &h));
			continue;
		}
		phmap_ref(&planned, &h, &created);
		if (created)
		{
			px_grow((void **) &p->send, &p->sendcap, p->nsend + 1, sizeof(GenEnt));
			p->send[p->nsend].gen = c.generation;
			p->send[p->nsend].h = h;
			p->nsend++;
		}
		for (uint32_t i = 0; i < c.nparents; i++)
		{
			if (c.parent_gen[i] > b)
			{
				px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
				sbound = px_realloc(sbound, cap * sizeof(uint64_t));
				stack[n] = c.parents[i];
				sbound[n++] = b;
			}
			else if (!phmap_find(&dst->loc, &c.parents[i]))
				plan_bound(p, &c.parents[i], src->name);
		}
	}
	/* a boundary that some other path sends is no boundary */
	{
		size_t		k = 0;

		for (size_t i = 0; i < p->nbound; i++)
			if (!phmap_find(&planned, &p->bound[i]))
			{
				p->bound[k] = p->bound[i];
				p->bhint[k++] = p->bhint[i];
			}
		p->nbound = k;
	}
	if (p->nsend > 1)
		qsort(p->send, p->nsend, sizeof(GenEnt), gen_cmp);
	free(stack);
	free(sbound);
	phmap_free(&seen);
	phmap_free(&planned);
}

/*
 * Histories must attach: walking back from the heads has to reach a commit
 * dst already knows, wherever it is there (a cut commit with the same hash
 * is the same history).  Everything down to it is sent.  If the walk ends
 * at roots without that, the histories are unrelated; if it ends at the
 * sender's own cut, they cannot be shown to attach until that history is
 * restored.  Only a clone (allow_unrelated) skips this, and stops at depth.
 */
static int
plan_transfer(VcRepo *src, VcRepo *dst, const PHash *heads, size_t nheads,
			  uint32_t depth, bool allow_unrelated, Plan *p)
{
	memset(p, 0, sizeof(*p));
	plan_walk(src, dst, heads, nheads, 0, p);
	if (p->connected || (p->nsend == 0 && !p->cut))
		return VC_OK;
	if (allow_unrelated)
	{
		if (depth > 0)
		{
			plan_free(p);
			plan_walk(src, dst, heads, nheads, depth, p);
		}
		return VC_OK;
	}
	if (p->cut)
	{
		src->ctx.err_hash = dst->ctx.err_hash = p->first_cut;
		plan_free(p);
		return VC_ERR_DETACHED;
	}
	plan_free(p);
	return VC_ERR_UNRELATED;
}

/* Copies the planned commits into dst, pinned there but not yet held. */
static int
plan_copy(VcRepo *src, VcRepo *dst, const Plan *p)
{
	for (size_t i = 0; i < p->nsend; i++)
	{
		PHash		bad;
		int			rc = cs_copy_closure(src->ctx.cs, &dst->ctx.pins,
										 &p->send[i].h, true, &bad);

		if (rc != CS_OK)
		{
			dst->ctx.err_hash = bad;
			prolly_release_pins(&dst->ctx);
			return rc;
		}
	}
	return VC_OK;
}

/* Takes the copied commits into dst's window history and records boundaries. */
static void
plan_adopt(VcRepo *dst, const Plan *p)
{
	for (size_t i = 0; i < p->nsend; i++)
	{
		cs_ref_root(dst->ctx.cs, &p->send[i].h);
		enter_window(dst, &p->send[i].h);
	}
	for (size_t i = 0; i < p->nbound; i++)
	{
		bool		created;
		uint64_t   *v = phmap_ref(&dst->loc, &p->bound[i], &created);

		if (created)
			*v = LOC_BOUNDARY;
		if (vc_hint(dst, &p->bound[i]) == NULL)
			set_hint(dst, &p->bound[i], p->bhint[i]);
	}
	prolly_release_pins(&dst->ctx);
}

static int
transfer(VcRepo *src, VcRepo *dst, const PHash *heads, size_t nheads,
		 uint32_t depth, bool allow_unrelated)
{
	Plan		p;
	int			rc = plan_transfer(src, dst, heads, nheads, depth,
								   allow_unrelated, &p);

	if (rc != VC_OK)
		return rc;
	rc = plan_copy(src, dst, &p);
	if (rc == VC_OK)
		plan_adopt(dst, &p);
	plan_free(&p);
	return rc;
}

static void
move_branch(VcRepo *r, VcBranch *b, const PHash *head, const PHash *root)
{
	move_head(r, &b->head, head);
	b->head = *head;
	b->head_root = *root;
	set_working(r, b, root);
}

/*
 * The remote checks fast-forward itself, against its own history plus the
 * commits just received (pinned, not yet held), so it never needs history
 * the sender has cut.  A rejected push leaves nothing behind.
 */
int
vc_push(VcRepo *local, VcRepo *remote, const char *branch)
{
	VcBranch   *lb = vc_branch_find(local, branch);
	VcBranch   *rb = vc_branch_find(remote, branch);
	Plan		p;
	PHash		base;
	int			rc;

	if (lb == NULL)
		return VC_ERR;
	if (rb && phash_eq(&rb->head, &lb->head))
		return VC_OK;
	if ((rc = plan_transfer(local, remote, &lb->head, 1, 0, false, &p)) != VC_OK)
		return rc;
	if ((rc = plan_copy(local, remote, &p)) != VC_OK)
	{
		plan_free(&p);
		return rc;
	}
	if (rb != NULL)
	{
		rc = vc_merge_base(&remote->ctx, &lb->head, &rb->head, &base);
		if (rc != 1 || !phash_eq(&base, &rb->head))
		{
			prolly_release_pins(&remote->ctx);
			plan_free(&p);
			return rc < 0 ? rc : VC_ERR;
		}
	}
	plan_adopt(remote, &p);
	plan_free(&p);
	rb = vc_branch_find(remote, branch);
	if (rb == NULL)
		add_branch(remote, branch, &lb->head, &lb->head_root);
	else
		move_branch(remote, rb, &lb->head, &lb->head_root);
	return VC_OK;
}

int
vc_fetch(VcRepo *local, VcRepo *remote, const char *branch, PHash *head)
{
	VcBranch   *rb = vc_branch_find(remote, branch);
	int			rc;

	if (rb == NULL)
		return VC_ERR;
	if ((rc = transfer(remote, local, &rb->head, 1, 0, false)) != VC_OK)
		return rc;
	*head = rb->head;
	return VC_OK;
}

int
vc_repo_clone(VcRepo *dst, ChunkStore *cs, VcRepo *src, uint32_t depth)
{
	PHash	   *heads = px_malloc(src->nbr * sizeof(PHash));
	int			rc;

	repo_init_empty(dst, cs, &src->ctx.policy);
	dst->clock = src->clock;
	dst->window_depth = depth;
	for (size_t i = 0; i < src->nbr; i++)
		heads[i] = src->br[i].head;
	rc = transfer(src, dst, heads, src->nbr, depth, true);
	free(heads);
	if (rc != VC_OK)
		return rc;
	for (size_t i = 0; i < src->nbr; i++)
		add_branch(dst, src->br[i].name, &src->br[i].head, &src->br[i].head_root);
	dst->cur = src->cur;
	prolly_release_pins(&dst->ctx);
	return VC_OK;
}

/*
 * Fetches history past a cut from any chunk source (a remote's store, an
 * archive, another repo), verified by hash, into a new local segment: old
 * history comes back without entering the window.  The walk starts at
 * `from` and goes back `generations` (0: as far as the source has), stopping
 * at commits this repo has content for.
 */
int
vc_deepen(VcRepo *r, const VcSource *src, const PHash *from,
		  uint32_t generations, uint32_t *seg_out)
{
	ProllyCtx	sctx;
	PHash	   *stack = NULL;
	size_t		n = 0,
				cap = 0;
	PHashMap	seen;
	GenEnt	   *todo = NULL;
	size_t		ntodo = 0,
				todocap = 0;
	PHash	   *bounds = NULL;
	size_t		nb = 0,
				bcap = 0;
	VcCommit	c;
	uint64_t	bound;
	bool		created;
	int			rc = VC_OK;
	VcLocation	where = vc_locate(r, from, NULL);
	VcSegment  *seg;

	*seg_out = VC_NO_SEGMENT;
	if (where != VC_LOC_UNKNOWN && where != VC_LOC_BOUNDARY &&
		where != VC_LOC_DISCARDED)
		return VC_ERR;			/* present here, or exported: attach it */
	prolly_ctx_init(&sctx, src->cs, &r->ctx.policy);
	if (!vc_commit_load(&sctx, from, &c))
	{
		prolly_ctx_free(&sctx);
		r->ctx.err_hash = *from;
		return VC_ERR_SEVERED;
	}
	bound = generations && c.generation > generations ? c.generation - generations : 0;
	phmap_init(&seen);
	px_grow((void **) &stack, &cap, 1, sizeof(PHash));
	stack[n++] = *from;
	while (n > 0)
	{
		PHash		h = stack[--n];
		VcLocation	l = vc_locate(r, &h, NULL);

		if (l == VC_LOC_WINDOW || l == VC_LOC_SEGMENT || l == VC_LOC_EXPORTED)
			continue;
		phmap_ref(&seen, &h, &created);
		if (!created)
			continue;
		if (!vc_commit_load(&sctx, &h, &c))
			continue;			/* the source lacks it: stays a boundary */
		px_grow((void **) &todo, &todocap, ntodo + 1, sizeof(GenEnt));
		todo[ntodo].gen = c.generation;
		todo[ntodo].h = h;
		ntodo++;
		for (uint32_t i = 0; i < c.nparents; i++)
		{
			if (c.parent_gen[i] > bound)
			{
				px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
				stack[n++] = c.parents[i];
			}
			else if (vc_locate(r, &c.parents[i], NULL) == VC_LOC_UNKNOWN ||
					 vc_locate(r, &c.parents[i], NULL) == VC_LOC_BOUNDARY)
			{
				px_grow((void **) &bounds, &bcap, nb + 1, sizeof(PHash));
				bounds[nb++] = c.parents[i];
			}
		}
	}
	if (ntodo > 1)
		qsort(todo, ntodo, sizeof(GenEnt), gen_cmp);
	for (size_t i = 0; i < ntodo && rc == VC_OK; i++)
	{
		PHash		bad;

		rc = cs_copy_closure(src->cs, &r->ctx.pins, &todo[i].h, true, &bad);
		if (rc != CS_OK)
			r->ctx.err_hash = bad;
	}
	if (rc == VC_OK && ntodo > 0)
	{
		px_grow((void **) &r->segs, &r->segcap, r->nsegs + 1, sizeof(VcSegment));
		seg = &r->segs[r->nsegs];
		memset(seg, 0, sizeof(*seg));
		seg->state = VC_SEG_LOCAL;
		seg->commits = px_malloc(ntodo * sizeof(PHash));
		seg->n = ntodo;
		for (size_t i = 0; i < ntodo; i++)
		{
			seg->commits[i] = todo[i].h;
			cs_ref_root(r->ctx.cs, &todo[i].h);
			set_loc(r, &todo[i].h, LOC_SEG_BASE + r->nsegs);
		}
		for (size_t i = 0; i < nb; i++)
		{
			uint64_t   *v = phmap_ref(&r->loc, &bounds[i], &created);

			if (created)
				*v = LOC_BOUNDARY;
			if (vc_hint(r, &bounds[i]) == NULL)
				set_hint(r, &bounds[i], src->name);
		}
		*seg_out = (uint32_t) r->nsegs++;
	}
	prolly_release_pins(&r->ctx);
	prolly_ctx_free(&sctx);
	free(stack);
	free(todo);
	free(bounds);
	phmap_free(&seen);
	return rc;
}

/*
 * Fetches branch from remote into a tracking branch "<remote>/<branch>"
 * and merges it into the current branch.  If the merge base lies past this
 * repo's cut and max_deepen > 0, deepens from the remote in growing steps,
 * up to max_deepen generations in all, until the merge can see its base.
 */
int
vc_pull(VcRepo *local, VcRepo *remote, const char *branch, uint32_t max_deepen,
		VcMergeResult *res)
{
	PHash		head;
	char		tracking[256];
	VcBranch   *tb;
	VcCommit	c;
	uint32_t	step = 4,
				used = 0;
	int			rc;

	memset(res, 0, sizeof(*res));
	if ((rc = vc_fetch(local, remote, branch, &head)) != VC_OK)
		return rc;
	snprintf(tracking, sizeof(tracking), "%s/%s", remote->name, branch);
	if ((rc = load_commit(&local->ctx, &head, &c)) != VC_OK)
		return rc;
	tb = vc_branch_find(local, tracking);
	if (tb == NULL)
		add_branch(local, tracking, &head, &c.root);
	else
		move_branch(local, tb, &head, &c.root);
	for (;;)
	{
		VcSource	s = {remote->name, remote->ctx.cs};
		PHash		missing;
		uint32_t	g,
					seg;

		rc = vc_merge(local, tracking, res);
		if (rc != VC_ERR_SEVERED || used >= max_deepen)
			return rc;
		missing = local->ctx.err_hash;
		g = step < max_deepen - used ? step : max_deepen - used;
		if (vc_deepen(local, &s, &missing, g, &seg) != VC_OK ||
			seg == VC_NO_SEGMENT)
			return rc;
		used += g;
		step *= 2;
	}
}
