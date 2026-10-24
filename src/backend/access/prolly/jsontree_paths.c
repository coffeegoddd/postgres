/*-------------------------------------------------------------------------
 *
 * jsontree_paths.c
 *	  JSON tree implementation "paths": a document as a tree of its scalars
 *	  keyed by path; see prolly_jsontree.h.
 *
 * Items.  A key is the scalar's path, a step after another:
 *
 *	0x01, uint32 length (big-endian), bytes		an object's key
 *	0x02, uint32 index (big-endian)				an array's element
 *
 * so keys compared as bytes (the tree's order) sort as jsonb orders the
 * document: an object's keys shorter first and then by bytes, an array's
 * elements by index, and a path before the longer ones it begins.  A
 * value is a kind byte and its bytes:
 *
 *	'n' null, 'f' false, 't' true
 *	's' a string's UTF-8 bytes
 *	'd' a numeric's bytes, as jsonb holds it (after its varlena header)
 *	'{' an empty object, '[' an empty array
 *
 * A container that holds anything has no item of its own: its items say
 * it is there.  A document that is one scalar, or an empty container, is
 * one item at the empty path.  The tree is the tree implementation's
 * ("cow"), its keys ordered by their bytes; a string longer than the
 * tree's inline limit goes out of line in a value tree as any long value.
 *
 * Reading.  Items stream into pushJsonbValue() in order: the steps a path
 * shares with the one before stay open, the others are closed, and those
 * after are opened, each container by the kind of step into it.  What is
 * read is checked as it comes: keys in increasing order, an array's
 * indexes from 0 without gaps, no item under another (a scalar, or an
 * empty container, holds nothing), strings in UTF-8, numerics as numeric
 * builds them.
 *
 * Editing.  A path's items are those whose keys begin with its bytes (a
 * step is self-delimiting, so a key that begins with a path's bytes is
 * under that path).  set() reads the items under the path it sets, deletes
 * them and puts the new value's, with apply(): the nodes on their paths are
 * all it writes.  Indexes are part of keys, so inserting into an array, or
 * removing from it, would move every element after; set() only replaces
 * elements.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/jsontree_paths.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "varatt.h"

#define STEP_KEY		0x01
#define STEP_INDEX		0x02
#define STEP_HEADER		5		/* a step's tag and its uint32 */

#define VAL_NULL		'n'
#define VAL_FALSE		'f'
#define VAL_TRUE		't'
#define VAL_STRING		's'
#define VAL_NUMERIC		'd'
#define VAL_OBJECT		'{'
#define VAL_ARRAY		'['

static void
bad_tree(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid prolly JSON tree: %s", what)));
}

static const ProllyTreeRoutine *
doc_tree(void)
{
	static const ProllyTreeRoutine *tree = NULL;

	if (tree == NULL)
		tree = prolly_tree_lookup("cow");
	return tree;
}

/* A document tree's environment: env's components, keys by their bytes. */
static void
doc_env(const ProllyTreeEnv *env, ProllyTreeEnv *denv)
{
	*denv = *env;
	denv->compare = prolly_bytes_compare;
	denv->compare_arg = NULL;
	denv->value_refs = NULL;
	denv->value_refs_arg = NULL;
	denv->chunk_refs = NULL;
	denv->chunk_refs_arg = NULL;
}

static void
put_u32(StringInfo out, uint32 v)
{
	char		b[4];

	b[0] = (char) (v >> 24);
	b[1] = (char) (v >> 16);
	b[2] = (char) (v >> 8);
	b[3] = (char) v;
	appendBinaryStringInfo(out, b, 4);
}

static uint32
get_u32(const uint8 *p)
{
	return ((uint32) p[0] << 24) | ((uint32) p[1] << 16) |
		((uint32) p[2] << 8) | (uint32) p[3];
}

static void
put_key_step(StringInfo path, const char *key, int len)
{
	appendStringInfoChar(path, STEP_KEY);
	put_u32(path, (uint32) len);
	appendBinaryStringInfo(path, key, len);
}

static void
put_index_step(StringInfo path, uint32 index)
{
	appendStringInfoChar(path, STEP_INDEX);
	put_u32(path, index);
}

/* A scalar's value bytes. */
static void
scalar_value(const JsonbValue *v, StringInfo out)
{
	switch (v->type)
	{
		case jbvNull:
			appendStringInfoChar(out, VAL_NULL);
			break;
		case jbvBool:
			appendStringInfoChar(out, v->val.boolean ? VAL_TRUE : VAL_FALSE);
			break;
		case jbvString:
			appendStringInfoChar(out, VAL_STRING);
			appendBinaryStringInfo(out, v->val.string.val, v->val.string.len);
			break;
		case jbvNumeric:
			appendStringInfoChar(out, VAL_NUMERIC);
			appendBinaryStringInfo(out, VARDATA_ANY(v->val.numeric),
								   VARSIZE_ANY_EXHDR(v->val.numeric));
			break;
		default:
			elog(ERROR, "a jsonb scalar of unknown type %d", (int) v->type);
	}
}

/* ------------------------------------------------------------------------
 * A document's items, in order
 * ------------------------------------------------------------------------
 */

typedef void (*EmitItem) (const StringInfo key, const StringInfo value,
						  void *arg);

typedef struct FlatFrame
{
	int			base;			/* the path's length at the container */
	bool		array;
	bool		raw;			/* a raw scalar's array */
	uint32		next;			/* an array's next index */
	uint32		children;
} FlatFrame;

/*
 * Emit the items of the document c holds, each key prefixed by prefix's
 * bytes (the path the document is at).
 */
static void
flatten(JsonbContainer *c, const char *prefix, int prefixlen, EmitItem emit,
		void *arg)
{
	JsonbIterator *it = JsonbIteratorInit(c);
	JsonbIteratorToken tok;
	JsonbValue	v;
	StringInfoData path;
	StringInfoData val;
	FlatFrame  *frames = NULL;
	int			nframes = 0;
	int			maxframes = 0;

	initStringInfo(&path);
	initStringInfo(&val);
	appendBinaryStringInfo(&path, prefix, prefixlen);
	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		FlatFrame  *top = nframes > 0 ? &frames[nframes - 1] : NULL;

		switch (tok)
		{
			case WJB_BEGIN_OBJECT:
			case WJB_BEGIN_ARRAY:
				if (top && top->array)
				{
					path.len = top->base;
					put_index_step(&path, top->next++);
					top->children++;
				}
				if (nframes == maxframes)
				{
					maxframes = Max(8, maxframes * 2);
					frames = frames ? repalloc(frames, sizeof(FlatFrame) * maxframes) :
						palloc(sizeof(FlatFrame) * maxframes);
				}
				frames[nframes].base = path.len;
				frames[nframes].array = (tok == WJB_BEGIN_ARRAY);
				frames[nframes].raw = (tok == WJB_BEGIN_ARRAY &&
									   v.val.array.rawScalar);
				frames[nframes].next = 0;
				frames[nframes].children = 0;
				nframes++;
				break;
			case WJB_KEY:
				path.len = top->base;
				put_key_step(&path, v.val.string.val, v.val.string.len);
				top->children++;
				break;
			case WJB_VALUE:
				resetStringInfo(&val);
				scalar_value(&v, &val);
				emit(&path, &val, arg);
				break;
			case WJB_ELEM:
				if (!top->raw)
				{
					path.len = top->base;
					put_index_step(&path, top->next++);
				}
				top->children++;
				resetStringInfo(&val);
				scalar_value(&v, &val);
				emit(&path, &val, arg);
				break;
			case WJB_END_OBJECT:
			case WJB_END_ARRAY:
				if (top->children == 0)
				{
					path.len = top->base;
					resetStringInfo(&val);
					appendStringInfoChar(&val, tok == WJB_END_OBJECT ?
										 VAL_OBJECT : VAL_ARRAY);
					emit(&path, &val, arg);
				}
				path.len = top->base;
				nframes--;
				break;
			default:
				elog(ERROR, "unexpected jsonb iterator token %d", (int) tok);
		}
	}
	if (frames)
		pfree(frames);
	pfree(path.data);
	pfree(val.data);
}

/* ------------------------------------------------------------------------
 * Items back into a document
 * ------------------------------------------------------------------------
 */

typedef struct Step
{
	int			off;			/* within the key */
	int			len;			/* the step's bytes, its header among them */
	uint8		tag;
	uint32		n;				/* a key's length, or an index */
} Step;

typedef struct RebuildFrame
{
	bool		array;
	uint32		next;			/* an array's next index */
} RebuildFrame;

typedef struct Rebuild
{
	JsonbInState st;
	int			strip;			/* the bytes of the path read under */
	bool		any;
	bool		done;			/* the document was one item */
	StringInfoData prev;		/* the key before, and its steps */
	Step	   *prev_steps;
	int			prev_n;
	Step	   *steps;
	int			maxsteps;
	RebuildFrame *frames;
	int			depth;			/* open containers */
} Rebuild;

static void
rebuild_init(Rebuild *rb, int strip)
{
	memset(rb, 0, sizeof(*rb));
	rb->strip = strip;
	initStringInfo(&rb->prev);
	rb->maxsteps = 16;
	rb->steps = palloc(sizeof(Step) * rb->maxsteps);
	rb->prev_steps = palloc(sizeof(Step) * rb->maxsteps);
	rb->frames = palloc(sizeof(RebuildFrame) * (rb->maxsteps + 1));
}

/* key's steps past the path read under into rb->steps; how many. */
static int
parse_steps(Rebuild *rb, const uint8 *key, int len)
{
	int			n = 0;
	int			p = rb->strip;

	while (p < len)
	{
		Step	   *s;

		if (n == rb->maxsteps)
		{
			rb->maxsteps *= 2;
			rb->steps = repalloc(rb->steps, sizeof(Step) * rb->maxsteps);
			rb->prev_steps = repalloc(rb->prev_steps, sizeof(Step) * rb->maxsteps);
			rb->frames = repalloc(rb->frames, sizeof(RebuildFrame) * (rb->maxsteps + 1));
		}
		s = &rb->steps[n++];
		if (len - p < STEP_HEADER)
			bad_tree("a path step cut short");
		s->off = p;
		s->tag = key[p];
		s->n = get_u32(key + p + 1);
		if (s->tag == STEP_KEY)
		{
			if (s->n > (uint32) (len - p - STEP_HEADER))
				bad_tree("an object key runs past its path");
			s->len = STEP_HEADER + (int) s->n;
		}
		else if (s->tag == STEP_INDEX)
			s->len = STEP_HEADER;
		else
			bad_tree("a path step of no kind");
		p += s->len;
	}
	return n;
}

/* A copy of a scalar's value bytes as a JsonbValue, checked. */
static void
value_scalar(const uint8 *val, int len, JsonbValue *v)
{
	if (len < 1)
		bad_tree("an item without a value");
	switch (val[0])
	{
		case VAL_NULL:
		case VAL_FALSE:
		case VAL_TRUE:
			if (len != 1)
				bad_tree("bytes after a null or boolean");
			if (val[0] == VAL_NULL)
				v->type = jbvNull;
			else
			{
				v->type = jbvBool;
				v->val.boolean = (val[0] == VAL_TRUE);
			}
			break;
		case VAL_STRING:
			if (!pg_verify_mbstr(PG_UTF8, (const char *) val + 1, len - 1, true))
				bad_tree("a string that isn't UTF-8");
			v->type = jbvString;
			v->val.string.len = len - 1;
			v->val.string.val = palloc(len);
			memcpy(v->val.string.val, val + 1, len - 1);
			v->val.string.val[len - 1] = '\0';
			break;
		case VAL_NUMERIC:
			{
				Numeric		num = palloc(VARHDRSZ + len - 1);

				SET_VARSIZE(num, VARHDRSZ + len - 1);
				memcpy(VARDATA(num), val + 1, len - 1);
				if (len - 1 < (int) sizeof(uint16) || !numeric_is_canonical(num))
					bad_tree("a numeric that isn't one numeric builds");
				v->type = jbvNumeric;
				v->val.numeric = num;
				break;
			}
		default:
			bad_tree("a value of no kind");
	}
}

static void
rebuild_open(Rebuild *rb, uint8 tag)
{
	RebuildFrame *f = &rb->frames[rb->depth++];

	f->array = (tag == STEP_INDEX);
	f->next = 0;
	pushJsonbValue(&rb->st, f->array ? WJB_BEGIN_ARRAY : WJB_BEGIN_OBJECT, NULL);
}

static void
rebuild_close(Rebuild *rb)
{
	RebuildFrame *f = &rb->frames[--rb->depth];

	pushJsonbValue(&rb->st, f->array ? WJB_END_ARRAY : WJB_END_OBJECT, NULL);
}

/* An empty container, or a scalar, pushed as a value (seq) */
static void
push_value(Rebuild *rb, JsonbIteratorToken seq, const uint8 *val, int len)
{
	JsonbValue	v;

	if (len == 1 && (val[0] == VAL_OBJECT || val[0] == VAL_ARRAY))
	{
		bool		object = (val[0] == VAL_OBJECT);

		pushJsonbValue(&rb->st, object ? WJB_BEGIN_OBJECT : WJB_BEGIN_ARRAY, NULL);
		pushJsonbValue(&rb->st, object ? WJB_END_OBJECT : WJB_END_ARRAY, NULL);
		return;
	}
	value_scalar(val, len, &v);
	if (seq == WJB_DONE)
	{
		/* a document that is a scalar: jsonb's raw scalar array */
		JsonbValue	a;

		a.type = jbvArray;
		a.val.array.rawScalar = true;
		a.val.array.nElems = 1;
		pushJsonbValue(&rb->st, WJB_BEGIN_ARRAY, &a);
		pushJsonbValue(&rb->st, WJB_ELEM, &v);
		pushJsonbValue(&rb->st, WJB_END_ARRAY, NULL);
		return;
	}
	pushJsonbValue(&rb->st, seq, &v);
}

static void
rebuild_add(Rebuild *rb, const uint8 *key, int keylen, const uint8 *val,
			int vallen)
{
	int			n = parse_steps(rb, key, keylen);
	int			common = 0;
	Step	   *swap;

	if (rb->done)
		bad_tree("an item after a document that is one value");
	if (rb->any)
	{
		int			r = memcmp(rb->prev.data, key, Min(rb->prev.len, keylen));

		if (r > 0 || (r == 0 && rb->prev.len >= keylen))
			bad_tree("paths out of order");
		while (common < n && common < rb->prev_n &&
			   rb->steps[common].len == rb->prev_steps[common].len &&
			   memcmp(key + rb->steps[common].off,
					  rb->prev.data + rb->prev_steps[common].off,
					  rb->steps[common].len) == 0)
			common++;
		if (common == rb->prev_n)
			bad_tree("an item under a scalar or an empty container");
		if (n == 0)
			bad_tree("a document's value with other items");
		while (rb->depth > common + 1)
			rebuild_close(rb);
	}
	else
	{
		rb->any = true;
		if (n == 0)
		{
			push_value(rb, WJB_DONE, val, vallen);
			rb->done = true;
			return;
		}
		rebuild_open(rb, rb->steps[0].tag);
	}

	for (int i = common; i < n; i++)
	{
		Step	   *s = &rb->steps[i];
		RebuildFrame *f = &rb->frames[i];

		if (f->array != (s->tag == STEP_INDEX))
			bad_tree("an object's key and an array's index in one container");
		if (s->tag == STEP_KEY)
		{
			JsonbValue	k;

			if (!pg_verify_mbstr(PG_UTF8, (const char *) key + s->off + STEP_HEADER,
								 (int) s->n, true))
				bad_tree("an object key that isn't UTF-8");
			k.type = jbvString;
			k.val.string.len = (int) s->n;
			k.val.string.val = palloc(s->n + 1);
			memcpy(k.val.string.val, key + s->off + STEP_HEADER, s->n);
			k.val.string.val[s->n] = '\0';
			pushJsonbValue(&rb->st, WJB_KEY, &k);
		}
		else
		{
			if (s->n != f->next)
				bad_tree("an array's indexes with a gap");
			f->next++;
		}
		if (i < n - 1)
			rebuild_open(rb, rb->steps[i + 1].tag);
		else
			push_value(rb, f->array ? WJB_ELEM : WJB_VALUE, val, vallen);
	}

	resetStringInfo(&rb->prev);
	appendBinaryStringInfo(&rb->prev, (const char *) key, keylen);
	swap = rb->prev_steps;
	rb->prev_steps = rb->steps;
	rb->steps = swap;
	rb->prev_n = n;
}

/* The document read, or NULL if no item was. */
static Jsonb *
rebuild_finish(Rebuild *rb)
{
	Jsonb	   *result = NULL;

	while (rb->depth > 0)
		rebuild_close(rb);
	if (rb->any)
		result = JsonbValueToJsonb(rb->st.result);
	pfree(rb->prev.data);
	pfree(rb->steps);
	pfree(rb->prev_steps);
	pfree(rb->frames);
	return result;
}

/* ------------------------------------------------------------------------
 * Build and read
 * ------------------------------------------------------------------------
 */

typedef struct BuildState
{
	ProllyTreeBuilder *b;
	Size		key_max;
	bool		too_long;
} BuildState;

static void
emit_build(const StringInfo key, const StringInfo value, void *arg)
{
	BuildState *bs = arg;
	ProllyKey	k = {(const uint8 *) key->data, (uint32) key->len};
	ProllyValue v = {(const uint8 *) value->data, (uint32) value->len};

	if (bs->too_long || (Size) key->len > bs->key_max)
	{
		bs->too_long = true;
		return;
	}
	PROLLY_CALL(doc_tree(), build_add, bs->b, &k, &v);
}

static bool
paths_build(const ProllyTreeEnv *env, Jsonb *doc, ProllyNodeRef *root)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	BuildState	bs;

	doc_env(env, &denv);
	bs.b = PROLLY_CALL(tree, build_begin, &denv, CurrentMemoryContext);
	bs.key_max = PROLLY_CALL(tree, key_max, denv.chunk_format, denv.hash);
	bs.too_long = false;
	flatten(&doc->root, "", 0, emit_build, &bs);
	PROLLY_CALL(tree, build_finish, bs.b, root);
	return !bs.too_long;
}

/*
 * The document under path prefix (len bytes) of the tree at cursor c,
 * read from its first item on; NULL if it has none.
 */
static Jsonb *
read_under(ProllyTreeCursor *c, const char *prefix, int len)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyKey	seek = {(const uint8 *) prefix, (uint32) len};
	ProllyKey	k;
	ProllyValue v;
	Rebuild		rb;

	rebuild_init(&rb, len);
	PROLLY_CALL(tree, cursor_seek, c, len > 0 ? &seek : NULL,
				ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if ((int) k.len < len || memcmp(k.data, prefix, len) != 0)
			break;
		CHECK_FOR_INTERRUPTS();
		rebuild_add(&rb, k.data, (int) k.len, v.data, (int) v.len);
	}
	return rebuild_finish(&rb);
}

static Jsonb *
paths_read(const ProllyTreeEnv *env, const ProllyNodeRef *root)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	ProllyTreeCursor *c;
	Jsonb	   *doc;

	doc_env(env, &denv);
	c = PROLLY_CALL(tree, cursor_open, &denv, root, CurrentMemoryContext);
	doc = read_under(c, "", 0);
	PROLLY_CALL(tree, cursor_close, c);
	if (doc == NULL)
		bad_tree("a document of no items");
	return doc;
}

/* ------------------------------------------------------------------------
 * Paths
 * ------------------------------------------------------------------------
 */

/* What a path's first item says is there. */
typedef enum PathKind
{
	PATH_NONE,					/* nothing */
	PATH_OBJECT,				/* an object that holds something */
	PATH_ARRAY,					/* an array that does */
	PATH_EMPTY_OBJECT,
	PATH_EMPTY_ARRAY,
	PATH_SCALAR,
} PathKind;

static PathKind
path_kind(ProllyTreeCursor *c, const StringInfo path)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyKey	seek = {(const uint8 *) path->data, (uint32) path->len};
	ProllyKey	k;
	ProllyValue v;

	PROLLY_CALL(tree, cursor_seek, c, path->len > 0 ? &seek : NULL,
				ForwardScanDirection);
	if (!PROLLY_CALL(tree, cursor_next, c, &k, &v) ||
		(int) k.len < path->len || memcmp(k.data, path->data, path->len) != 0)
		return PATH_NONE;
	if ((int) k.len > path->len)
	{
		if (k.data[path->len] == STEP_KEY)
			return PATH_OBJECT;
		if (k.data[path->len] == STEP_INDEX)
			return PATH_ARRAY;
		bad_tree("a path step of no kind");
	}
	if (v.len == 1 && v.data[0] == VAL_OBJECT)
		return PATH_EMPTY_OBJECT;
	if (v.len == 1 && v.data[0] == VAL_ARRAY)
		return PATH_EMPTY_ARRAY;
	return PATH_SCALAR;
}

/*
 * An array index as #> and jsonb_set read one: false if it isn't an
 * integer; *index the integer (negative counts from the end).
 */
static bool
path_index(const char *s, int *index)
{
	char	   *end;

	errno = 0;
	*index = strtoint(s, &end, 10);
	return end != s && *end == '\0' && errno == 0;
}

/* What descending a path step found. */
typedef enum Descent
{
	DESCENT_FOUND,				/* path names a step */
	DESCENT_NOWHERE,			/* nothing is there, or no step goes there */
	DESCENT_NOT_HANDLED,		/* a step only a whole read handles */
	DESCENT_NOT_INTEGER,		/* an array's step that isn't an integer */
} Descent;

/* The step for elem from the container at path (of kind), onto path. */
static Descent
descend(PathKind kind, const char *elem, StringInfo path)
{
	int			index;

	switch (kind)
	{
		case PATH_OBJECT:
			put_key_step(path, elem, strlen(elem));
			return DESCENT_FOUND;
		case PATH_ARRAY:
			if (!path_index(elem, &index))
				return DESCENT_NOT_INTEGER;
			if (index < 0)
				return DESCENT_NOT_HANDLED;
			put_index_step(path, (uint32) index);
			return DESCENT_FOUND;
		default:
			return DESCENT_NOWHERE;
	}
}

static bool
paths_get(const ProllyTreeEnv *env, const ProllyNodeRef *root, char **path,
		  int npath, Jsonb **result)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	ProllyTreeCursor *c;
	StringInfoData p;
	bool		handled = true;

	doc_env(env, &denv);
	c = PROLLY_CALL(tree, cursor_open, &denv, root, CurrentMemoryContext);
	initStringInfo(&p);
	*result = NULL;
	for (int i = 0; i < npath; i++)
	{
		Descent		d = descend(path_kind(c, &p), path[i], &p);

		if (d == DESCENT_NOT_HANDLED)
			handled = false;
		if (d != DESCENT_FOUND)
			break;
		if (i == npath - 1)
			*result = read_under(c, p.data, p.len);
	}
	if (npath == 0)
		*result = read_under(c, "", 0);
	PROLLY_CALL(tree, cursor_close, c);
	pfree(p.data);
	return handled;
}

/* ------------------------------------------------------------------------
 * Set
 * ------------------------------------------------------------------------
 */

typedef struct KeyList
{
	StringInfoData bytes;		/* the keys' and values' bytes, one after another */
	int		   *key_off;
	int		   *key_len;
	int		   *val_off;
	int		   *val_len;
	int			n;
	int			max;
} KeyList;

static void
keylist_init(KeyList *kl)
{
	initStringInfo(&kl->bytes);
	kl->n = 0;
	kl->max = 16;
	kl->key_off = palloc(sizeof(int) * kl->max);
	kl->key_len = palloc(sizeof(int) * kl->max);
	kl->val_off = palloc(sizeof(int) * kl->max);
	kl->val_len = palloc(sizeof(int) * kl->max);
}

static void
keylist_add(KeyList *kl, const uint8 *key, int keylen, const uint8 *val,
			int vallen)
{
	if (kl->n == kl->max)
	{
		kl->max *= 2;
		kl->key_off = repalloc(kl->key_off, sizeof(int) * kl->max);
		kl->key_len = repalloc(kl->key_len, sizeof(int) * kl->max);
		kl->val_off = repalloc(kl->val_off, sizeof(int) * kl->max);
		kl->val_len = repalloc(kl->val_len, sizeof(int) * kl->max);
	}
	kl->key_off[kl->n] = kl->bytes.len;
	kl->key_len[kl->n] = keylen;
	appendBinaryStringInfo(&kl->bytes, (const char *) key, keylen);
	kl->val_off[kl->n] = kl->bytes.len;
	kl->val_len[kl->n] = vallen;
	appendBinaryStringInfo(&kl->bytes, (const char *) val, vallen);
	kl->n++;
}

static void
emit_keylist(const StringInfo key, const StringInfo value, void *arg)
{
	keylist_add((KeyList *) arg, (const uint8 *) key->data, key->len,
				(const uint8 *) value->data, value->len);
}

static ProllyKey
keylist_key(const KeyList *kl, int i)
{
	ProllyKey	k = {(const uint8 *) kl->bytes.data + kl->key_off[i],
	(uint32) kl->key_len[i]};

	return k;
}

static ProllyValue
keylist_value(const KeyList *kl, int i)
{
	ProllyValue v = {(const uint8 *) kl->bytes.data + kl->val_off[i],
	(uint32) kl->val_len[i]};

	return v;
}

/* The items under path, read into kl; and their document into *doc. */
static void
read_items_under(ProllyTreeCursor *c, const StringInfo path, KeyList *kl,
				 Jsonb **doc)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyKey	seek = {(const uint8 *) path->data, (uint32) path->len};
	ProllyKey	k;
	ProllyValue v;
	Rebuild		rb;

	rebuild_init(&rb, path->len);
	PROLLY_CALL(tree, cursor_seek, c, &seek, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if ((int) k.len < path->len || memcmp(k.data, path->data, path->len) != 0)
			break;
		CHECK_FOR_INTERRUPTS();
		keylist_add(kl, k.data, (int) k.len, v.data, (int) v.len);
		rebuild_add(&rb, k.data, (int) k.len, v.data, (int) v.len);
	}
	*doc = rebuild_finish(&rb);
}

static int
key_cmp(const ProllyKey *a, const ProllyKey *b)
{
	return prolly_bytes_compare(a, b, NULL);
}

static bool
paths_set(const ProllyTreeEnv *env, const ProllyNodeRef *root, char **path,
		  int npath, Jsonb *value, bool create, ProllyNodeRef *new_root,
		  int64 *delta)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	ProllyTreeCursor *c;
	StringInfoData p;
	PathKind	kind = PATH_NONE;
	int			parent_len;
	KeyList		old;
	KeyList		new;
	Jsonb	   *old_doc;
	ProllyEdit *edits;
	int			nedits = 0;
	int			i;
	int			j;
	Descent		d;

	*new_root = *root;
	*delta = 0;
	if (npath == 0)
		return true;			/* jsonb_set gives the document back */

	doc_env(env, &denv);
	c = PROLLY_CALL(tree, cursor_open, &denv, root, CurrentMemoryContext);
	initStringInfo(&p);

	/* the container the last step is in */
	for (i = 0; i < npath; i++)
	{
		kind = path_kind(c, &p);
		if (i == 0 && kind == PATH_SCALAR)
		{
			/* (jsonb_set's error: "cannot set path in scalar") */
			PROLLY_CALL(tree, cursor_close, c);
			return false;
		}
		if (i == npath - 1)
			break;
		d = descend(kind, path[i], &p);
		if (d == DESCENT_NOT_HANDLED || d == DESCENT_NOT_INTEGER)
		{
			PROLLY_CALL(tree, cursor_close, c);
			return false;
		}
		if (d == DESCENT_NOWHERE)
		{
			PROLLY_CALL(tree, cursor_close, c);
			return true;		/* nothing on the path: no change */
		}
	}
	parent_len = p.len;

	if (kind == PATH_OBJECT || kind == PATH_EMPTY_OBJECT)
		put_key_step(&p, path[npath - 1], strlen(path[npath - 1]));
	else if (kind == PATH_ARRAY || kind == PATH_EMPTY_ARRAY)
	{
		int			index;

		/* only an element that is there is replaced here */
		if (!path_index(path[npath - 1], &index) || index < 0 ||
			kind == PATH_EMPTY_ARRAY)
		{
			PROLLY_CALL(tree, cursor_close, c);
			return false;
		}
		put_index_step(&p, (uint32) index);
	}
	else
	{
		/* a scalar on the path, or nothing: no change */
		PROLLY_CALL(tree, cursor_close, c);
		return true;
	}

	keylist_init(&old);
	read_items_under(c, &p, &old, &old_doc);
	PROLLY_CALL(tree, cursor_close, c);
	if (old.n == 0 && (kind == PATH_ARRAY || !create))
		return kind != PATH_ARRAY;	/* past the array's end, or no key */

	keylist_init(&new);
	flatten(&value->root, p.data, p.len, emit_keylist, &new);

	/* The old items deleted, the new put, by key; an empty object's item. */
	edits = palloc(sizeof(ProllyEdit) * (old.n + new.n + 1));
	if (kind == PATH_EMPTY_OBJECT)
	{
		edits[nedits].key.data = (const uint8 *) p.data;
		edits[nedits].key.len = (uint32) parent_len;
		edits[nedits].is_delete = true;
		nedits++;
	}
	for (i = 0, j = 0; i < old.n || j < new.n;)
	{
		ProllyKey	ok = i < old.n ? keylist_key(&old, i) : (ProllyKey) {0};
		ProllyKey	nk = j < new.n ? keylist_key(&new, j) : (ProllyKey) {0};
		int			r = i >= old.n ? 1 : j >= new.n ? -1 : key_cmp(&ok, &nk);

		if (r < 0)
		{
			edits[nedits].key = ok;
			edits[nedits].is_delete = true;
			i++;
		}
		else
		{
			ProllyValue nv = keylist_value(&new, j);

			if (r == 0)
			{
				ProllyValue ov = keylist_value(&old, i);

				i++;
				j++;
				if (ov.len == nv.len && memcmp(ov.data, nv.data, nv.len) == 0)
					continue;	/* the same item */
			}
			else
				j++;
			edits[nedits].key = nk;
			edits[nedits].value = nv;
			edits[nedits].is_delete = false;
		}
		nedits++;
	}
	if (nedits > 0)
		PROLLY_CALL(tree, apply, &denv, root, edits, nedits, new_root);

	/* the document's size changes by the value's, and a new key's entry */
	*delta = (int64) VARSIZE(value) - (old_doc ? (int64) VARSIZE(old_doc) :
									   -(int64) (strlen(path[npath - 1]) +
												 2 * sizeof(JEntry)));
	pfree(edits);
	return true;
}

/* ------------------------------------------------------------------------
 * Diff
 * ------------------------------------------------------------------------
 */

/* key's path elements, in the current memory context. */
static char **
key_path(const uint8 *key, int len, int *npath)
{
	Rebuild		rb;
	char	  **path;
	int			n;

	rebuild_init(&rb, 0);
	n = parse_steps(&rb, key, len);
	path = palloc(sizeof(char *) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		Step	   *s = &rb.steps[i];

		if (s->tag == STEP_KEY)
			path[i] = pnstrdup((const char *) key + s->off + STEP_HEADER, s->n);
		else
			path[i] = psprintf("%u", s->n);
	}
	*npath = n;
	pfree(rb.prev.data);
	pfree(rb.steps);
	pfree(rb.prev_steps);
	pfree(rb.frames);
	return path;
}

/* A value's bytes as a document (NULL for none). */
static Jsonb *
value_doc(const ProllyValue *v)
{
	Rebuild		rb;

	if (v == NULL)
		return NULL;
	rebuild_init(&rb, 0);
	rebuild_add(&rb, (const uint8 *) "", 0, v->data, (int) v->len);
	return rebuild_finish(&rb);
}

typedef struct DiffState
{
	ProllyJsonChangeVisit visit;
	void	   *arg;
	MemoryContext cxt;			/* reset after each change */
} DiffState;

static void
report(DiffState *ds, ProllyDiffKind kind, const ProllyKey *key,
	   const ProllyValue *before, const ProllyValue *after)
{
	MemoryContext old = MemoryContextSwitchTo(ds->cxt);
	int			npath;
	char	  **path = key_path(key->data, (int) key->len, &npath);

	ds->visit(kind, path, npath, value_doc(before), value_doc(after), ds->arg);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ds->cxt);
}

static void
diff_change(ProllyDiffKind kind, const ProllyKey *key,
			const ProllyValue *before, const ProllyValue *after, void *arg)
{
	report((DiffState *) arg, kind, key, before, after);
}

static void
paths_diff(const ProllyTreeEnv *env, const ProllyNodeRef *from,
		   const ProllyNodeRef *to, ProllyJsonChangeVisit visit, void *arg)
{
	ProllyTreeEnv denv;
	DiffState	ds;

	doc_env(env, &denv);
	ds.visit = visit;
	ds.arg = arg;
	ds.cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly JSON diff",
								   ALLOCSET_DEFAULT_SIZES);
	PROLLY_CALL(doc_tree(), diff, &denv, from, to, diff_change, &ds);
	MemoryContextDelete(ds.cxt);
}

static void
paths_diff_documents(Jsonb *from, Jsonb *to, ProllyJsonChangeVisit visit,
					 void *arg)
{
	KeyList		a;
	KeyList		b;
	DiffState	ds;
	int			i = 0;
	int			j = 0;

	keylist_init(&a);
	keylist_init(&b);
	flatten(&from->root, "", 0, emit_keylist, &a);
	flatten(&to->root, "", 0, emit_keylist, &b);
	ds.visit = visit;
	ds.arg = arg;
	ds.cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly JSON diff",
								   ALLOCSET_DEFAULT_SIZES);
	while (i < a.n || j < b.n)
	{
		ProllyKey	ak = i < a.n ? keylist_key(&a, i) : (ProllyKey) {0};
		ProllyKey	bk = j < b.n ? keylist_key(&b, j) : (ProllyKey) {0};
		int			r = i >= a.n ? 1 : j >= b.n ? -1 : key_cmp(&ak, &bk);

		if (r < 0)
		{
			ProllyValue av = keylist_value(&a, i++);

			report(&ds, PROLLY_DIFF_REMOVED, &ak, &av, NULL);
		}
		else if (r > 0)
		{
			ProllyValue bv = keylist_value(&b, j++);

			report(&ds, PROLLY_DIFF_ADDED, &bk, NULL, &bv);
		}
		else
		{
			ProllyValue av = keylist_value(&a, i++);
			ProllyValue bv = keylist_value(&b, j++);

			if (av.len != bv.len || memcmp(av.data, bv.data, av.len) != 0)
				report(&ds, PROLLY_DIFF_MODIFIED, &ak, &av, &bv);
		}
	}
	MemoryContextDelete(ds.cxt);
}

const ProllyJsonTreeRoutine prolly_jsontree_paths = {
	.info = {
		.iface = PROLLY_IFACE_JSON_TREE,
		.impl_id = 1,
		.format_version = 1,
		.name = "paths",
		.description = "a document's scalars keyed by path in jsonb's order",
	},
	.build = paths_build,
	.read = paths_read,
	.get = paths_get,
	.set = paths_set,
	.diff = paths_diff,
	.diff_documents = paths_diff_documents,
};
