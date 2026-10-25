/*-------------------------------------------------------------------------
 *
 * jsontree_paths.c
 *	  JSON tree implementation "paths": a document as a tree keyed by path,
 *	  its short containers packed; see prolly_jsontree.h.
 *
 * Items.  A key is a path, a step after another:
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
 *	'j' a container's jsonb bytes (its header, entries and children)
 *
 * A container of PROLLY_JSON_PACK_MAX bytes or fewer is a 'j' item; a
 * longer one is its children's items (so it holds one at least).  The
 * document's own container is always longer.  A string longer than the
 * tree's inline limit goes out of line in a value tree as any long value.
 * The bytes of a container are the same wherever it is in a document: a
 * container starts on a 4-byte boundary, and the padding within it is to
 * 4-byte boundaries.
 *
 * Reading.  Items stream into containers being laid out as jsonb lays
 * them out (convertJsonbObject() and convertJsonbArray()): the steps a
 * path shares with the one before stay open, the others are closed, each
 * then laid out into its parent, and those after are opened.  A packed
 * container's bytes are copied as they are.  What is read is checked as
 * it comes (keys in order, an array's indexes from 0 without gaps, nothing
 * under an item, each container packed exactly when it is short enough),
 * and the document read as canonical jsonb bytes are checked.
 *
 * Editing.  set() finds the item holding the path, or the longer container
 * it names: it replaces the items under the path with the new value's (a
 * packed container's edited with jsonb_set()), then packs each container
 * above that has become short enough, with apply().  The nodes on those
 * items' paths are all it writes, and the containers above it reads only
 * until they are known to be long.  Indexes are part of keys, so inserting
 * into an array, or removing from it, would move every element after;
 * set() only replaces elements of an array that isn't packed.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/jsontree_paths.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "catalog/pg_type.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
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
#define VAL_CONTAINER	'j'

/* (as a walk's pack_max) no container packed but the empty ones */
#define PACK_NONE		(-1)

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

/* A uint32 of jsonb's binary at p, in the machine's order. */
static inline uint32
jb_u32(const char *p)
{
	uint32		v;

	memcpy(&v, p, sizeof(v));
	return v;
}

/* ------------------------------------------------------------------------
 * A path's steps
 * ------------------------------------------------------------------------
 */

typedef struct Step
{
	int			off;			/* within the key */
	int			len;			/* the step's bytes, its header among them */
	uint8		tag;
	uint32		n;				/* a key's length, or an index */
} Step;

typedef struct Steps
{
	Step	   *s;
	int			n;
	int			max;
} Steps;

static void
steps_init(Steps *st)
{
	st->max = 16;
	st->n = 0;
	st->s = palloc(sizeof(Step) * st->max);
}

/* key's steps from byte from on into st. */
static void
parse_steps(Steps *st, const uint8 *key, int len, int from)
{
	int			p = from;

	st->n = 0;
	while (p < len)
	{
		Step	   *s;

		if (st->n == st->max)
		{
			st->max *= 2;
			st->s = repalloc(st->s, sizeof(Step) * st->max);
		}
		s = &st->s[st->n++];
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
}

/* ------------------------------------------------------------------------
 * A document's items, in order
 * ------------------------------------------------------------------------
 */

typedef void (*EmitItem) (const StringInfo key, const StringInfo value,
						  void *arg);

typedef struct Walk
{
	StringInfoData path;
	StringInfoData val;
	int			pack_max;		/* or PACK_NONE */
	EmitItem	emit;
	void	   *arg;
} Walk;

static void walk_container(Walk *w, const char *doc, uint32 at, uint32 end);

static void
walk_init(Walk *w, const char *prefix, int prefixlen, int pack_max,
		  EmitItem emit, void *arg)
{
	initStringInfo(&w->path);
	initStringInfo(&w->val);
	appendBinaryStringInfo(&w->path, prefix, prefixlen);
	w->pack_max = pack_max;
	w->emit = emit;
	w->arg = arg;
}

static void
walk_done(Walk *w)
{
	pfree(w->path.data);
	pfree(w->val.data);
}

/*
 * The item, or items, of the value doc[from, stop) of entry type type,
 * at w's path; doc is a document's binary, from its start.
 */
static void
walk_value(Walk *w, const char *doc, uint32 from, uint32 stop, uint32 type)
{
	resetStringInfo(&w->val);
	switch (type)
	{
		case JENTRY_ISSTRING:
			appendStringInfoChar(&w->val, VAL_STRING);
			appendBinaryStringInfo(&w->val, doc + from, stop - from);
			break;
		case JENTRY_ISNUMERIC:
			{
				uint32		at = INTALIGN(from);

				if (stop < at + VARHDRSZ)
					elog(ERROR, "a jsonb numeric cut short");
				appendStringInfoChar(&w->val, VAL_NUMERIC);
				appendBinaryStringInfo(&w->val, doc + at + VARHDRSZ,
									   stop - at - VARHDRSZ);
				break;
			}
		case JENTRY_ISBOOL_FALSE:
			appendStringInfoChar(&w->val, VAL_FALSE);
			break;
		case JENTRY_ISBOOL_TRUE:
			appendStringInfoChar(&w->val, VAL_TRUE);
			break;
		case JENTRY_ISNULL:
			appendStringInfoChar(&w->val, VAL_NULL);
			break;
		case JENTRY_ISCONTAINER:
			{
				uint32		at = INTALIGN(from);
				uint32		count = jb_u32(doc + at) & JB_CMASK;

				if ((w->pack_max != PACK_NONE && stop - at > (uint32) w->pack_max) ||
					(w->pack_max == PACK_NONE && count > 0))
				{
					walk_container(w, doc, at, stop);
					return;
				}
				appendStringInfoChar(&w->val, VAL_CONTAINER);
				appendBinaryStringInfo(&w->val, doc + at, stop - at);
				break;
			}
		default:
			elog(ERROR, "a jsonb entry of unknown type %u", type);
	}
	w->emit(&w->path, &w->val, w->arg);
}

/* The items of the container doc[at, end), each at w's path and its step. */
static void
walk_container(Walk *w, const char *doc, uint32 at, uint32 end)
{
	uint32		header = jb_u32(doc + at);
	uint32		count = header & JB_CMASK;
	bool		object = (header & JB_FOBJECT) != 0;
	uint32		nchildren = object ? count * 2 : count;
	uint32		data = at + sizeof(uint32) + nchildren * sizeof(JEntry);
	uint32	   *ends = palloc(sizeof(uint32) * Max(nchildren, 1));
	uint32		prev = 0;
	int			base = w->path.len;

	check_stack_depth();
	for (uint32 i = 0; i < nchildren; i++)
	{
		JEntry		e = jb_u32(doc + at + sizeof(uint32) + i * sizeof(JEntry));

		prev = JBE_HAS_OFF(e) ? JBE_OFFLENFLD(e) : prev + JBE_OFFLENFLD(e);
		ends[i] = prev;
	}
	if (data + prev > end)
		elog(ERROR, "a jsonb container runs past its entry");
	for (uint32 i = 0; i < count; i++)
	{
		uint32		vi = object ? count + i : i;
		JEntry		e = jb_u32(doc + at + sizeof(uint32) + vi * sizeof(JEntry));

		w->path.len = base;
		if (object)
		{
			uint32		kstart = i == 0 ? 0 : ends[i - 1];

			put_key_step(&w->path, doc + data + kstart, (int) (ends[i] - kstart));
		}
		else
			put_index_step(&w->path, i);
		walk_value(w, doc, data + (vi == 0 ? 0 : ends[vi - 1]), data + ends[vi],
				   e & JENTRY_TYPEMASK);
		CHECK_FOR_INTERRUPTS();
	}
	w->path.len = base;
	pfree(ends);
}

/*
 * The items of jb (a document, or a value to put at w's path): a scalar is
 * one item, as a container short enough to pack is.
 */
static void
walk_jsonb(Walk *w, Jsonb *jb)
{
	const char *doc = VARDATA(jb);
	uint32		len = VARSIZE(jb) - VARHDRSZ;
	uint32		header = jb_u32(doc);

	if ((header & JB_FSCALAR) != 0)
	{
		JEntry		e = jb_u32(doc + sizeof(uint32));
		uint32		data = sizeof(uint32) + sizeof(JEntry);

		walk_value(w, doc, data, data + JBE_OFFLENFLD(e), e & JENTRY_TYPEMASK);
		return;
	}
	if ((w->pack_max != PACK_NONE && len <= (uint32) w->pack_max) ||
		(w->pack_max == PACK_NONE && (header & JB_CMASK) == 0))
	{
		resetStringInfo(&w->val);
		appendStringInfoChar(&w->val, VAL_CONTAINER);
		appendBinaryStringInfo(&w->val, doc, len);
		w->emit(&w->path, &w->val, w->arg);
		return;
	}
	walk_container(w, doc, 0, len);
}

/* ------------------------------------------------------------------------
 * Items back into a document
 * ------------------------------------------------------------------------
 */

typedef struct LayFrame LayFrame;

/*
 * A container being read: its children's keys and values, a child that is
 * a container of items of its own kept as its frame, so that each byte is
 * copied once into the frames and once into the document.
 */
struct LayFrame
{
	bool		object;
	uint32		n;				/* children */
	uint32		max;
	uint32	   *klen;			/* an object's keys' lengths */
	uint32	   *vlen;			/* values' bytes, without padding */
	uint8	   *vtype;			/* their entries' types */
	LayFrame  **sub;			/* a child's frame, or NULL */
	StringInfoData keys;
	StringInfoData vals;		/* the values but the frames' */
	uint32		laid;			/* (closed) its length laid out */
};

typedef struct Assemble
{
	int			strip;			/* the bytes of the path read under */
	bool		any;
	LayFrame  **frames;			/* the open containers, outermost first */
	int			depth;
	int			maxdepth;
	Steps		steps;
	Steps		prev_steps;
	StringInfoData prev;		/* the key before */
} Assemble;

static void
assemble_init(Assemble *as, int strip)
{
	memset(as, 0, sizeof(*as));
	as->strip = strip;
	as->maxdepth = 8;
	as->frames = palloc(sizeof(LayFrame *) * as->maxdepth);
	steps_init(&as->steps);
	steps_init(&as->prev_steps);
	initStringInfo(&as->prev);
}

static LayFrame *
frame_open(Assemble *as, uint8 tag)
{
	LayFrame   *f = palloc0(sizeof(LayFrame));

	if (as->depth == as->maxdepth)
	{
		as->maxdepth *= 2;
		as->frames = repalloc(as->frames, sizeof(LayFrame *) * as->maxdepth);
	}
	as->frames[as->depth++] = f;
	f->max = 16;
	f->klen = palloc(sizeof(uint32) * f->max);
	f->vlen = palloc(sizeof(uint32) * f->max);
	f->vtype = palloc(f->max);
	f->sub = palloc(sizeof(LayFrame *) * f->max);
	initStringInfo(&f->keys);
	initStringInfo(&f->vals);
	f->object = (tag == STEP_KEY);
	return f;
}

/* A child of f at step s (of key), its value to come. */
static void
frame_child(LayFrame *f, const Step *s, const uint8 *key)
{
	if (f->object != (s->tag == STEP_KEY))
		bad_tree("an object's key and an array's index in one container");
	if (!f->object && s->n != f->n)
		bad_tree("an array's indexes with a gap");
	if (f->n == f->max)
	{
		f->max *= 2;
		f->klen = repalloc(f->klen, sizeof(uint32) * f->max);
		f->vlen = repalloc(f->vlen, sizeof(uint32) * f->max);
		f->vtype = repalloc(f->vtype, f->max);
		f->sub = repalloc(f->sub, sizeof(LayFrame *) * f->max);
	}
	if (f->object)
	{
		appendBinaryStringInfo(&f->keys, (const char *) key + s->off + STEP_HEADER,
							   (int) s->n);
		f->klen[f->n] = s->n;
	}
	f->vlen[f->n] = 0;
	f->vtype[f->n] = 0;
	f->sub[f->n] = NULL;
	f->n++;
}

/* An item's value as f's last child. */
static void
frame_value(LayFrame *f, const uint8 *val, int len)
{
	uint32		type;
	int			start = f->vals.len;

	if (len < 1)
		bad_tree("an item without a value");
	switch (val[0])
	{
		case VAL_NULL:
		case VAL_FALSE:
		case VAL_TRUE:
			if (len != 1)
				bad_tree("bytes after a null or boolean");
			type = val[0] == VAL_NULL ? JENTRY_ISNULL :
				val[0] == VAL_TRUE ? JENTRY_ISBOOL_TRUE : JENTRY_ISBOOL_FALSE;
			break;
		case VAL_STRING:
			type = JENTRY_ISSTRING;
			appendBinaryStringInfo(&f->vals, (const char *) val + 1, len - 1);
			break;
		case VAL_NUMERIC:
			{
				uint32		hdr[2];	/* (as long as a varlena's header is read) */

				type = JENTRY_ISNUMERIC;
				SET_VARSIZE(hdr, VARHDRSZ + len - 1);
				appendBinaryStringInfo(&f->vals, (const char *) hdr, VARHDRSZ);
				appendBinaryStringInfo(&f->vals, (const char *) val + 1, len - 1);
				break;
			}
		case VAL_CONTAINER:
			if (len - 1 < (int) sizeof(uint32))
				bad_tree("a container without a header");
			if (len - 1 > PROLLY_JSON_PACK_MAX)
				bad_tree("a container too long to be one item, as one");
			type = JENTRY_ISCONTAINER;
			appendBinaryStringInfo(&f->vals, (const char *) val + 1, len - 1);
			break;
		default:
			bad_tree("a value of no kind");
			type = 0;			/* keep compiler quiet */
	}
	f->vtype[f->n - 1] = (uint8) (type >> 28);
	f->vlen[f->n - 1] = (uint32) (f->vals.len - start);
}

static void
offlen_check(uint64 total)
{
	if (total > JENTRY_OFFLENMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("total size of jsonb container elements exceeds the maximum of %d bytes",
						JENTRY_OFFLENMASK)));
}

/* Whether a value of entry type type is padded to a 4-byte boundary. */
static inline bool
padded(uint32 type)
{
	return type == JENTRY_ISNUMERIC || type == JENTRY_ISCONTAINER;
}

/* f's length laid out, its frames' laid already. */
static uint32
frame_length(LayFrame *f)
{
	uint64		rel = sizeof(uint32) +
		(uint64) (f->object ? f->n * 2 : f->n) * sizeof(JEntry) + f->keys.len;

	for (uint32 i = 0; i < f->n; i++)
	{
		if (padded((uint32) f->vtype[i] << 28))
			rel = INTALIGN(rel);
		rel += f->sub[i] ? f->sub[i]->laid : f->vlen[i];
		offlen_check(rel);
	}
	return (uint32) rel;
}

/*
 * f laid out at the end of dst, as convertJsonbObject() and
 * convertJsonbArray() lay a container out (the padding before it, which is
 * its parent's, left out).  dst has room for it.
 */
static void
frame_layout(LayFrame *f, StringInfo dst)
{
	int			start = dst->len;
	uint32		header = f->n | (f->object ? JB_FOBJECT : JB_FARRAY);
	uint32		nentries = f->object ? f->n * 2 : f->n;
	char	   *entries;
	uint32		total = 0;
	uint32		voff = 0;

	Assert(dst->maxlen - dst->len > (int) f->laid);
	memcpy(dst->data + dst->len, &header, sizeof(header));
	dst->len += sizeof(header);
	entries = dst->data + dst->len;
	dst->len += nentries * sizeof(JEntry);

	if (f->object)
	{
		memcpy(dst->data + dst->len, f->keys.data, f->keys.len);
		dst->len += f->keys.len;
	}
	for (uint32 i = 0; f->object && i < f->n; i++)
	{
		JEntry		meta;

		total += f->klen[i];
		meta = JENTRY_ISSTRING | ((i % JB_OFFSET_STRIDE) == 0 ?
								  (total | JENTRY_HAS_OFF) : f->klen[i]);
		memcpy(entries + i * sizeof(JEntry), &meta, sizeof(meta));
	}
	for (uint32 i = 0; i < f->n; i++)
	{
		uint32		type = (uint32) f->vtype[i] << 28;
		uint32		len = 0;
		uint32		index = f->object ? f->n + i : i;
		JEntry		meta;

		if (padded(type))
		{
			int			rel = dst->len - start;
			int			pad = INTALIGN(rel) - rel;

			memset(dst->data + dst->len, 0, pad);
			dst->len += pad;
			len = pad;
		}
		if (f->sub[i])
		{
			frame_layout(f->sub[i], dst);
			len += f->sub[i]->laid;
		}
		else
		{
			memcpy(dst->data + dst->len, f->vals.data + voff, f->vlen[i]);
			dst->len += f->vlen[i];
			voff += f->vlen[i];
			len += f->vlen[i];
		}
		total += len;
		meta = type | ((index % JB_OFFSET_STRIDE) == 0 ?
					   (total | JENTRY_HAS_OFF) : len);
		memcpy(entries + index * sizeof(JEntry), &meta, sizeof(meta));
	}
	Assert((uint32) (dst->len - start) == f->laid);
	dst->data[dst->len] = '\0';
}

/* The innermost open container closed, its parent's last child. */
static void
frame_close(Assemble *as, bool checked)
{
	LayFrame   *f = as->frames[--as->depth];
	LayFrame   *parent = as->frames[as->depth - 1];

	f->laid = frame_length(f);
	if (checked && f->laid <= PROLLY_JSON_PACK_MAX)
		bad_tree("a container short enough to be one item, as items");
	parent->vtype[parent->n - 1] = (uint8) (JENTRY_ISCONTAINER >> 28);
	parent->sub[parent->n - 1] = f;
}

static void
assemble_add(Assemble *as, const uint8 *key, int keylen, const uint8 *val,
			 int vallen)
{
	Steps	   *st = &as->steps;
	int			keep = 1;		/* the containers kept open */
	Steps		swap;

	parse_steps(st, key, keylen, as->strip);
	if (st->n == 0)
		bad_tree("an item where its container is");
	if (as->any)
	{
		Steps	   *pv = &as->prev_steps;
		int			r = memcmp(as->prev.data, key, Min(as->prev.len, keylen));
		int			common = 0;

		if (r > 0 || (r == 0 && as->prev.len >= keylen))
			bad_tree("paths out of order");
		while (common < st->n && common < pv->n &&
			   st->s[common].len == pv->s[common].len &&
			   memcmp(key + st->s[common].off,
					  as->prev.data + pv->s[common].off,
					  st->s[common].len) == 0)
			common++;
		if (common == pv->n)
			bad_tree("an item under another");
		keep = Min(common, pv->n - 1) + 1;
		while (as->depth > keep)
			frame_close(as, true);
	}
	else
	{
		as->any = true;
		frame_open(as, st->s[0].tag);
	}

	for (int i = keep - 1; i < st->n; i++)
	{
		frame_child(as->frames[i], &st->s[i], key);
		if (i < st->n - 1)
			frame_open(as, st->s[i + 1].tag);
		else
			frame_value(as->frames[i], val, vallen);
	}

	resetStringInfo(&as->prev);
	appendBinaryStringInfo(&as->prev, (const char *) key, keylen);
	swap = as->prev_steps;
	as->prev_steps = as->steps;
	as->steps = swap;
}

/*
 * The container read, as jsonb, laid out in cxt; NULL if no item was.  If
 * limit isn't 0 and it is longer, NULL too.  checked: whether to check
 * the containers' lengths are a tree's.
 */
static Jsonb *
assemble_finish(Assemble *as, MemoryContext cxt, uint32 limit, bool checked)
{
	StringInfoData out;
	MemoryContext old;
	LayFrame   *root;
	Jsonb	   *jb;

	if (!as->any)
		return NULL;
	while (as->depth > 1)
		frame_close(as, checked);
	root = as->frames[0];
	root->laid = frame_length(root);
	if (limit > 0 && root->laid > limit)
		return NULL;
	old = MemoryContextSwitchTo(cxt);
	initStringInfoExt(&out, VARHDRSZ + root->laid + 1);
	MemoryContextSwitchTo(old);
	out.len = VARHDRSZ;
	frame_layout(root, &out);
	jb = (Jsonb *) out.data;
	SET_VARSIZE(jb, out.len);
	return jb;
}

/* A value's bytes as a document (NULL for none), checked. */
static Jsonb *
value_doc(const uint8 *val, int len)
{
	JsonbValue	v;

	if (val == NULL)
		return NULL;
	if (len < 1)
		bad_tree("an item without a value");
	switch (val[0])
	{
		case VAL_CONTAINER:
			{
				Jsonb	   *jb = palloc(VARHDRSZ + len - 1);

				SET_VARSIZE(jb, VARHDRSZ + len - 1);
				memcpy(VARDATA(jb), val + 1, len - 1);
				prolly_jsonb_check_utf8(VARDATA(jb), len - 1);
				return prolly_jsonb_from_utf8(jb);
			}
		case VAL_NULL:
		case VAL_FALSE:
		case VAL_TRUE:
			if (len != 1)
				bad_tree("bytes after a null or boolean");
			if (val[0] == VAL_NULL)
				v.type = jbvNull;
			else
			{
				v.type = jbvBool;
				v.val.boolean = (val[0] == VAL_TRUE);
			}
			break;
		case VAL_STRING:
			if (!pg_verify_mbstr(PG_UTF8, (const char *) val + 1, len - 1, true))
				bad_tree("a string that isn't UTF-8");
			v.type = jbvString;
			v.val.string.len = len - 1;
			v.val.string.val = pnstrdup((const char *) val + 1, len - 1);
			break;
		case VAL_NUMERIC:
			{
				Numeric		num = palloc(VARHDRSZ + len - 1);

				SET_VARSIZE(num, VARHDRSZ + len - 1);
				memcpy(VARDATA(num), val + 1, len - 1);
				if (len - 1 < (int) sizeof(uint16) || !numeric_is_canonical(num))
					bad_tree("a numeric that isn't one numeric builds");
				v.type = jbvNumeric;
				v.val.numeric = num;
				break;
			}
		default:
			bad_tree("a value of no kind");
	}
	return prolly_jsonb_from_utf8(JsonbValueToJsonb(&v));
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
paths_build(const ProllyTreeEnv *env, const char *doc, Size len,
			ProllyNodeRef *root)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	BuildState	bs;
	Walk		w;

	/* a scalar, or a document short enough to be inline, is no tree */
	if (len < sizeof(uint32) || len + 1 <= PROLLY_JSON_TREE_MIN ||
		(jb_u32(doc) & JB_FSCALAR) != 0)
		return false;
	doc_env(env, &denv);
	bs.b = PROLLY_CALL(tree, build_begin, &denv, CurrentMemoryContext);
	bs.key_max = PROLLY_CALL(tree, key_max, denv.chunk_format, denv.hash);
	bs.too_long = false;
	walk_init(&w, "", 0, PROLLY_JSON_PACK_MAX, emit_build, &bs);
	walk_container(&w, doc, 0, (uint32) len);
	walk_done(&w);
	PROLLY_CALL(tree, build_finish, bs.b, root);
	return !bs.too_long;
}

/*
 * The container under path prefix (len bytes) of the tree at cursor c,
 * read from its first item on, checked, in the database's encoding; NULL
 * if it has none.  document: the container is the document's.
 */
static Jsonb *
read_under(ProllyTreeCursor *c, const char *prefix, int len, bool document)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyKey	seek = {(const uint8 *) prefix, (uint32) len};
	ProllyKey	k;
	ProllyValue v;
	Assemble	as;
	Jsonb	   *result;
	MemoryContext caller = CurrentMemoryContext;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly JSON read",
											  ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(cxt);
	assemble_init(&as, len);
	PROLLY_CALL(tree, cursor_seek, c, len > 0 ? &seek : NULL,
				ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if ((int) k.len < len || memcmp(k.data, prefix, len) != 0)
			break;
		assemble_add(&as, k.data, (int) k.len, v.data, (int) v.len);
		CHECK_FOR_INTERRUPTS();
	}
	result = assemble_finish(&as, caller, 0, true);
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(cxt);
	if (result == NULL)
		return NULL;
	if (document ? VARSIZE(result) - VARHDRSZ + 1 <= PROLLY_JSON_TREE_MIN :
		VARSIZE(result) - VARHDRSZ <= PROLLY_JSON_PACK_MAX)
		bad_tree(document ? "a document too short to be a tree" :
				 "a container short enough to be one item, as items");
	prolly_jsonb_check_utf8(VARDATA(result), VARSIZE(result) - VARHDRSZ);
	return prolly_jsonb_from_utf8(result);
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
	doc = read_under(c, "", 0, true);
	PROLLY_CALL(tree, cursor_close, c);
	if (doc == NULL)
		bad_tree("a document of no items");
	return doc;
}

/* ------------------------------------------------------------------------
 * Paths
 * ------------------------------------------------------------------------
 */

/* What is at a path. */
typedef enum PathKind
{
	PATH_NONE,					/* nothing */
	PATH_OBJECT,				/* an object of items of its own */
	PATH_ARRAY,					/* an array of them */
	PATH_ITEM,					/* an item: a scalar, or a packed container */
} PathKind;

/* What is at path; an item's value copied into *item. */
static PathKind
path_kind(ProllyTreeCursor *c, const StringInfo path, StringInfo item)
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
	if (item)
	{
		resetStringInfo(item);
		appendBinaryStringInfo(item, (const char *) v.data, v.len);
	}
	return PATH_ITEM;
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
	DESCENT_NOT_HANDLED,		/* a step only a whole read handles */
	DESCENT_NOT_INTEGER,		/* an array's step that isn't an integer */
} Descent;

/* The step for elem from the container at path (of kind), onto path. */
static Descent
descend(PathKind kind, const char *elem, StringInfo path)
{
	int			index;

	if (kind == PATH_OBJECT)
	{
		put_key_step(path, elem, strlen(elem));
		return DESCENT_FOUND;
	}
	Assert(kind == PATH_ARRAY);
	if (!path_index(elem, &index))
		return DESCENT_NOT_INTEGER;
	if (index < 0)
		return DESCENT_NOT_HANDLED;
	put_index_step(path, (uint32) index);
	return DESCENT_FOUND;
}

/* path[0..n) as a text array. */
static ArrayType *
path_array(char **path, int n)
{
	Datum	   *elems = palloc(sizeof(Datum) * Max(n, 1));

	for (int i = 0; i < n; i++)
		elems[i] = CStringGetTextDatum(path[i]);
	return construct_array_builtin(elems, n, TEXTOID);
}

/*
 * Whether path[0..n) in container jb is a path jsonb_set() follows without
 * an error: through no array by an element that isn't an integer.
 */
static bool
path_settable(Jsonb *jb, char **path, int n)
{
	JsonbContainer *c = &jb->root;

	for (int i = 0; i < n; i++)
	{
		JsonbValue *v;
		JsonbValue	k;
		int			index;

		if (JsonContainerIsScalar(c))
			return true;
		if (JsonContainerIsObject(c))
		{
			k.type = jbvString;
			k.val.string.val = path[i];
			k.val.string.len = strlen(path[i]);
			v = findJsonbValueFromContainer(c, JB_FOBJECT, &k);
		}
		else
		{
			if (!path_index(path[i], &index))
				return false;
			if (index < 0)
				index += JsonContainerSize(c);
			v = index >= 0 ? getIthJsonbValueFromContainer(c, (uint32) index) : NULL;
		}
		if (v == NULL || v->type != jbvBinary)
			return true;
		c = v->val.binary.data;
	}
	return true;
}

static bool
paths_get(const ProllyTreeEnv *env, const ProllyNodeRef *root, char **path,
		  int npath, Jsonb **result)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	ProllyTreeCursor *c;
	StringInfoData p;
	StringInfoData item;
	PathKind	kind;
	bool		handled = true;

	doc_env(env, &denv);
	c = PROLLY_CALL(tree, cursor_open, &denv, root, CurrentMemoryContext);
	initStringInfo(&p);
	initStringInfo(&item);
	*result = NULL;
	kind = path_kind(c, &p, NULL);
	for (int i = 0; i < npath; i++)
	{
		Descent		d = descend(kind, path[i], &p);

		if (d != DESCENT_FOUND)
		{
			handled = (d != DESCENT_NOT_HANDLED);
			break;
		}
		kind = path_kind(c, &p, &item);
		if (kind == PATH_NONE)
			break;
		if (kind == PATH_ITEM)
		{
			Jsonb	   *v = value_doc((const uint8 *) item.data, item.len);

			if (i == npath - 1)
				*result = v;
			else if (item.data[0] == VAL_CONTAINER)
			{
				Datum	   *elems = palloc(sizeof(Datum) * (npath - i - 1));
				bool		isnull;
				Datum		r;

				for (int j = i + 1; j < npath; j++)
					elems[j - i - 1] = CStringGetTextDatum(path[j]);
				r = jsonb_get_element(v, elems, npath - i - 1, &isnull, false);
				if (!isnull)
					*result = DatumGetJsonbP(r);
			}
			break;
		}
		if (i == npath - 1)
			*result = read_under(c, p.data, p.len, false);
	}
	if (npath == 0)
		*result = read_under(c, "", 0, true);
	PROLLY_CALL(tree, cursor_close, c);
	pfree(p.data);
	pfree(item.data);
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

static int
key_cmp(const ProllyKey *a, const ProllyKey *b)
{
	return prolly_bytes_compare(a, b, NULL);
}

/* The items at path or under it, read into kl. */
static void
read_items_at(ProllyTreeCursor *c, const char *path, int len, KeyList *kl)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyKey	seek = {(const uint8 *) path, (uint32) len};
	ProllyKey	k;
	ProllyValue v;

	PROLLY_CALL(tree, cursor_seek, c, len > 0 ? &seek : NULL,
				ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if ((int) k.len < len || memcmp(k.data, path, len) != 0)
			break;
		CHECK_FOR_INTERRUPTS();
		keylist_add(kl, k.data, (int) k.len, v.data, (int) v.len);
	}
}

/*
 * The container of items under path (len bytes) if it is limit bytes or
 * shorter, read; NULL if it is longer.  Its items are read only until
 * they are known to make it longer: each adds at least its entry, its
 * value's bytes, and in an object its key and the key's entry.
 */
static Jsonb *
short_container(ProllyTreeCursor *c, const char *path, int len, uint32 limit)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyKey	seek = {(const uint8 *) path, (uint32) len};
	ProllyKey	k;
	ProllyValue v;
	KeyList		kl;
	uint64		least = sizeof(uint32);
	Steps		st;
	Assemble	as;
	Jsonb	   *jb;

	keylist_init(&kl);
	steps_init(&st);
	PROLLY_CALL(tree, cursor_seek, c, len > 0 ? &seek : NULL,
				ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		if ((int) k.len < len || memcmp(k.data, path, len) != 0)
			break;
		parse_steps(&st, k.data, (int) k.len, len);
		if (st.n == 0)
			bad_tree("an item where its container is");
		least += sizeof(JEntry) + (v.len > 0 ? v.len - 1 : 0);
		if (st.s[st.n - 1].tag == STEP_KEY)
			least += sizeof(JEntry) + st.s[st.n - 1].n;
		if (least > limit)
			return NULL;
		keylist_add(&kl, k.data, (int) k.len, v.data, (int) v.len);
	}
	assemble_init(&as, len);
	for (int i = 0; i < kl.n; i++)
	{
		ProllyKey	ik = keylist_key(&kl, i);
		ProllyValue iv = keylist_value(&kl, i);

		assemble_add(&as, ik.data, (int) ik.len, iv.data, (int) iv.len);
	}
	if (!as.any)
		bad_tree("a container of no items");
	/* (its containers' lengths not checked: one may be what is packed) */
	jb = assemble_finish(&as, CurrentMemoryContext, limit, false);
	if (jb != NULL)
		prolly_jsonb_check_utf8(VARDATA(jb), VARSIZE(jb) - VARHDRSZ);
	return jb;
}

/*
 * Edits making the items at path (len bytes) and under it those of new
 * instead of those of old, appended to edits.
 */
static int
replace_edits(const KeyList *old, const KeyList *new, ProllyEdit *edits)
{
	int			nedits = 0;

	for (int i = 0, j = 0; i < old->n || j < new->n;)
	{
		ProllyKey	ok = i < old->n ? keylist_key(old, i) : (ProllyKey) {0};
		ProllyKey	nk = j < new->n ? keylist_key(new, j) : (ProllyKey) {0};
		int			r = i >= old->n ? 1 : j >= new->n ? -1 : key_cmp(&ok, &nk);

		if (r < 0)
		{
			edits[nedits].key = ok;
			edits[nedits].is_delete = true;
			i++;
		}
		else
		{
			ProllyValue nv = keylist_value(new, j);

			if (r == 0)
			{
				ProllyValue ov = keylist_value(old, i);

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
	return nedits;
}

/* The values' bytes of kl's items. */
static int64
keylist_value_bytes(const KeyList *kl)
{
	int64		n = 0;

	for (int i = 0; i < kl->n; i++)
		n += kl->val_len[i] - 1;
	return n;
}

/*
 * The tree at *root with the value at path (len bytes) made value's: its
 * items there replaced, and each container above that has become short
 * enough packed.  False if a key would be too long for the tree.
 */
static bool
replace_at(const ProllyTreeEnv *denv, ProllyNodeRef *root, const char *path,
		   int len, Jsonb *value, int64 *delta)
{
	const ProllyTreeRoutine *tree = doc_tree();
	Size		key_max = PROLLY_CALL(tree, key_max, denv->chunk_format, denv->hash);
	ProllyTreeCursor *c;
	KeyList		old;
	KeyList		new;
	ProllyEdit *edits;
	int			nedits;
	Walk		w;
	Steps		st;

	keylist_init(&old);
	keylist_init(&new);
	walk_init(&w, path, len, PROLLY_JSON_PACK_MAX, emit_keylist, &new);
	walk_jsonb(&w, value);
	walk_done(&w);
	for (int i = 0; i < new.n; i++)
		if ((Size) new.key_len[i] > key_max)
			return false;

	c = PROLLY_CALL(tree, cursor_open, denv, root, CurrentMemoryContext);
	read_items_at(c, path, len, &old);
	PROLLY_CALL(tree, cursor_close, c);
	edits = palloc(sizeof(ProllyEdit) * (old.n + new.n + 1));
	nedits = replace_edits(&old, &new, edits);
	if (nedits > 0)
		PROLLY_CALL(tree, apply, denv, root, edits, nedits, root);
	*delta += keylist_value_bytes(&new) - keylist_value_bytes(&old);

	/* the containers above, from the innermost, packed if short enough */
	steps_init(&st);
	parse_steps(&st, (const uint8 *) path, len, 0);
	for (int depth = st.n - 1; depth >= 1 && nedits > 0; depth--)
	{
		int			plen = st.s[depth].off;
		Jsonb	   *packed;

		c = PROLLY_CALL(tree, cursor_open, denv, root, CurrentMemoryContext);
		packed = short_container(c, path, plen, PROLLY_JSON_PACK_MAX);
		if (packed == NULL)
		{
			PROLLY_CALL(tree, cursor_close, c);
			break;
		}
		resetStringInfo(&old.bytes);
		old.n = 0;
		read_items_at(c, path, plen, &old);
		PROLLY_CALL(tree, cursor_close, c);
		resetStringInfo(&new.bytes);
		new.n = 0;
		{
			StringInfoData v;

			initStringInfo(&v);
			appendStringInfoChar(&v, VAL_CONTAINER);
			appendBinaryStringInfo(&v, VARDATA(packed), VARSIZE(packed) - VARHDRSZ);
			keylist_add(&new, (const uint8 *) path, plen, (const uint8 *) v.data,
						v.len);
		}
		edits = repalloc(edits, sizeof(ProllyEdit) * (old.n + new.n + 1));
		nedits = replace_edits(&old, &new, edits);
		if (nedits > 0)
			PROLLY_CALL(tree, apply, denv, root, edits, nedits, root);
	}
	return true;
}

static bool
paths_set(const ProllyTreeEnv *env, const ProllyNodeRef *root, char **path,
		  int npath, Jsonb *value, bool create, ProllyNodeRef *new_root,
		  int64 *delta, Jsonb **whole)
{
	const ProllyTreeRoutine *tree = doc_tree();
	ProllyTreeEnv denv;
	ProllyTreeCursor *c;
	StringInfoData p;
	StringInfoData item;
	PathKind	kind;
	Jsonb	   *put = NULL;		/* what goes at p, if anything */
	int			index;

	*new_root = *root;
	*delta = 0;
	*whole = NULL;
	if (npath == 0)
		return true;			/* jsonb_set gives the document back */

	doc_env(env, &denv);
	c = PROLLY_CALL(tree, cursor_open, &denv, root, CurrentMemoryContext);
	initStringInfo(&p);
	initStringInfo(&item);
	kind = path_kind(c, &p, NULL);
	for (int i = 0; i < npath; i++)
	{
		bool		last = (i == npath - 1);

		if (kind == PATH_ARRAY)
		{
			/* only an element that is there is replaced here */
			if (!path_index(path[i], &index) || index < 0)
			{
				PROLLY_CALL(tree, cursor_close, c);
				return false;
			}
		}
		(void) descend(kind, path[i], &p);
		if (path_kind(c, &p, &item) == PATH_NONE)
		{
			/* past an array's end: only a whole edit appends */
			if (kind == PATH_ARRAY)
			{
				PROLLY_CALL(tree, cursor_close, c);
				return false;
			}
			if (last && create)
				put = value;	/* a key added */
			break;				/* (else nothing on the path: no change) */
		}
		kind = path_kind(c, &p, &item);
		if (last)
		{
			put = value;
			break;
		}
		if (kind == PATH_ITEM)
		{
			Jsonb	   *sub;

			if (item.data[0] != VAL_CONTAINER)
				break;			/* a path through a scalar: no change */
			sub = value_doc((const uint8 *) item.data, item.len);
			if (!path_settable(sub, path + i + 1, npath - i - 1))
			{
				PROLLY_CALL(tree, cursor_close, c);
				return false;
			}
			put = DatumGetJsonbP(DirectFunctionCall4(jsonb_set,
													 JsonbPGetDatum(sub),
													 PointerGetDatum(path_array(path + i + 1,
																				npath - i - 1)),
													 JsonbPGetDatum(value),
													 BoolGetDatum(create)));
			break;
		}
	}
	PROLLY_CALL(tree, cursor_close, c);
	if (put == NULL)
		return true;

	if (!replace_at(&denv, new_root, p.data, p.len, put, delta))
	{
		*new_root = *root;
		*delta = 0;
		return false;
	}

	/* a document edited short enough to be inline is no tree */
	c = PROLLY_CALL(tree, cursor_open, &denv, new_root, CurrentMemoryContext);
	*whole = short_container(c, "", 0, PROLLY_JSON_TREE_MIN - 1);
	PROLLY_CALL(tree, cursor_close, c);
	if (*whole != NULL)
		*whole = prolly_jsonb_from_utf8(*whole);
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
	Steps		st;
	char	  **path;

	steps_init(&st);
	parse_steps(&st, key, len, 0);
	path = palloc(sizeof(char *) * Max(st.n, 1));
	for (int i = 0; i < st.n; i++)
	{
		Step	   *s = &st.s[i];

		if (s->tag == STEP_KEY)
			path[i] = pnstrdup((const char *) key + s->off + STEP_HEADER, s->n);
		else
			path[i] = psprintf("%u", s->n);
	}
	*npath = st.n;
	pfree(st.s);
	return path;
}

typedef struct DiffState
{
	ProllyJsonChangeVisit visit;
	void	   *arg;
	MemoryContext cxt;			/* reset after each change */
	KeyList		from;			/* the scalars of the items changed */
	KeyList		to;
} DiffState;

static void
report(DiffState *ds, ProllyDiffKind kind, const ProllyKey *key,
	   const ProllyValue *before, const ProllyValue *after)
{
	MemoryContext old = MemoryContextSwitchTo(ds->cxt);
	int			npath;
	char	  **path = key_path(key->data, (int) key->len, &npath);

	ds->visit(kind, path, npath,
			  before ? value_doc(before->data, (int) before->len) : NULL,
			  after ? value_doc(after->data, (int) after->len) : NULL,
			  ds->arg);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ds->cxt);
}

/* Each scalar path whose value differs between a and b, reported. */
static void
report_scalars(DiffState *ds, const KeyList *a, const KeyList *b)
{
	int			i = 0;
	int			j = 0;

	while (i < a->n || j < b->n)
	{
		ProllyKey	ak = i < a->n ? keylist_key(a, i) : (ProllyKey) {0};
		ProllyKey	bk = j < b->n ? keylist_key(b, j) : (ProllyKey) {0};
		int			r = i >= a->n ? 1 : j >= b->n ? -1 : key_cmp(&ak, &bk);

		if (r < 0)
		{
			ProllyValue av = keylist_value(a, i++);

			report(ds, PROLLY_DIFF_REMOVED, &ak, &av, NULL);
		}
		else if (r > 0)
		{
			ProllyValue bv = keylist_value(b, j++);

			report(ds, PROLLY_DIFF_ADDED, &bk, NULL, &bv);
		}
		else
		{
			ProllyValue av = keylist_value(a, i++);
			ProllyValue bv = keylist_value(b, j++);

			if (av.len != bv.len || memcmp(av.data, bv.data, av.len) != 0)
				report(ds, PROLLY_DIFF_MODIFIED, &ak, &av, &bv);
		}
	}
}

/* An item's scalars, at its key, into kl: a packed container's each. */
static void
item_scalars(const ProllyKey *key, const ProllyValue *v, KeyList *kl)
{
	if (v->len > 1 && v->data[0] == VAL_CONTAINER &&
		(jb_u32((const char *) v->data + 1) & JB_CMASK) > 0)
	{
		Walk		w;

		walk_init(&w, (const char *) key->data, (int) key->len, PACK_NONE,
				  emit_keylist, kl);
		walk_container(&w, (const char *) v->data + 1, 0, v->len - 1);
		walk_done(&w);
		return;
	}
	keylist_add(kl, key->data, (int) key->len, v->data, (int) v->len);
}

static void
diff_change(ProllyDiffKind kind, const ProllyKey *key,
			const ProllyValue *before, const ProllyValue *after, void *arg)
{
	DiffState  *ds = arg;

	if (before)
		item_scalars(key, before, &ds->from);
	if (after)
		item_scalars(key, after, &ds->to);
}

static void
diff_init(DiffState *ds, ProllyJsonChangeVisit visit, void *arg)
{
	ds->visit = visit;
	ds->arg = arg;
	ds->cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly JSON diff",
									ALLOCSET_DEFAULT_SIZES);
	keylist_init(&ds->from);
	keylist_init(&ds->to);
}

/*
 * The items that differ between the trees, each one's scalars collected
 * (a packed container's, each), are compared scalar by scalar: an item of
 * one tree may be a container the other has as items.  Both lists are in
 * key order, since no item of a tree is under another.
 */
static void
paths_diff(const ProllyTreeEnv *env, const ProllyNodeRef *from,
		   const ProllyNodeRef *to, ProllyJsonChangeVisit visit, void *arg)
{
	ProllyTreeEnv denv;
	DiffState	ds;

	doc_env(env, &denv);
	diff_init(&ds, visit, arg);
	PROLLY_CALL(doc_tree(), diff, &denv, from, to, diff_change, &ds);
	report_scalars(&ds, &ds.from, &ds.to);
	MemoryContextDelete(ds.cxt);
}

static void
paths_diff_documents(Jsonb *from, Jsonb *to, ProllyJsonChangeVisit visit,
					 void *arg)
{
	DiffState	ds;
	Walk		w;

	diff_init(&ds, visit, arg);
	walk_init(&w, "", 0, PACK_NONE, emit_keylist, &ds.from);
	walk_jsonb(&w, from);
	walk_done(&w);
	walk_init(&w, "", 0, PACK_NONE, emit_keylist, &ds.to);
	walk_jsonb(&w, to);
	walk_done(&w);
	report_scalars(&ds, &ds.from, &ds.to);
	MemoryContextDelete(ds.cxt);
}

const ProllyJsonTreeRoutine prolly_jsontree_paths = {
	.info = {
		.iface = PROLLY_IFACE_JSON_TREE,
		.impl_id = 1,
		/* (pinned by canon_folded.c: change FOLDED_FORMAT with it) */
		.format_version = 2,	/* 2: short containers packed */
		.name = "paths",
		.description = "a document keyed by path in jsonb's order, short containers packed",
	},
	.build = paths_build,
	.read = paths_read,
	.get = paths_get,
	.set = paths_set,
	.diff = paths_diff,
	.diff_documents = paths_diff_documents,
};
