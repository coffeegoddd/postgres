/*-------------------------------------------------------------------------
 *
 * ws_obj.c
 *	  A versioned schema's objects other than tables (prolly/ws/ws_obj.h), and
 *	  "binary", their codec.
 *
 * Its key: kind's word, a slash, name, and a function's input types in parentheses.
 * Its value, each string a uvarint length and bytes: definition; a flags byte; those
 * input types; uvarint ndeps and keys; owner, empty for none; uvarint ncomments and each
 * one's column, empty for none, and text; uvarint ngrants and each one's column, empty
 * for none, grantee, empty for PUBLIC, grant option byte, privileges.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_obj.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "common/int.h"
#include "lib/binaryheap.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_target.h"
#include "parser/parser.h"
#include "prolly/ws/ws_obj.h"
#include "storage/lmgr.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_tags.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

const WsObjectCodecRoutine *WsObjectCodec = &BinaryWsObjectCodec;

#define OBJ_AFTER_TABLES 0x01
#define OBJ_PUBLIC_REVOKED 0x02
#define OBJ_FIRES_DISABLED 0x04
#define OBJ_FIRES_REPLICA 0x08
#define OBJ_FIRES_ALWAYS 0x10
#define OBJ_FLAGS (OBJ_AFTER_TABLES | OBJ_PUBLIC_REVOKED | OBJ_FIRES_DISABLED | OBJ_FIRES_REPLICA | OBJ_FIRES_ALWAYS)

static const char *const kind_words[] = {
	[WS_OBJ_CAST] = "cast",
	[WS_OBJ_COLLATION] = "collation",
	[WS_OBJ_CONVERSION] = "conversion",
	[WS_OBJ_OPERATOR] = "operator",
	[WS_OBJ_OPLINK] = "oplink",
	[WS_OBJ_OPCLASS] = "opclass",
	[WS_OBJ_OPFAMILY] = "opfamily",
	[WS_OBJ_OPMEMBERS] = "opmembers",
	[WS_OBJ_DEFAULT] = "default",
	[WS_OBJ_DOMAIN] = "domain",
	[WS_OBJ_FUNCTION] = "function",
	[WS_OBJ_INDEX] = "index",
	[WS_OBJ_MATVIEW] = "matview",
	[WS_OBJ_POLICY] = "policy",
	[WS_OBJ_PROCEDURE] = "procedure",
	[WS_OBJ_RULE] = "rule",
	[WS_OBJ_SEQUENCE] = "sequence",
	[WS_OBJ_TRIGGER] = "trigger",
	[WS_OBJ_TYPE] = "type",
	[WS_OBJ_VIEW] = "view",
};

static const char *const kind_sql[] = {
	[WS_OBJ_CAST] = "CAST",
	[WS_OBJ_COLLATION] = "COLLATION",
	[WS_OBJ_CONVERSION] = "CONVERSION",
	[WS_OBJ_OPERATOR] = "OPERATOR",
	[WS_OBJ_OPLINK] = "OPERATOR",
	[WS_OBJ_OPCLASS] = "OPERATOR CLASS",
	[WS_OBJ_OPFAMILY] = "OPERATOR FAMILY",
	[WS_OBJ_OPMEMBERS] = "OPERATOR FAMILY",
	[WS_OBJ_DEFAULT] = "DEFAULT",
	[WS_OBJ_DOMAIN] = "DOMAIN",
	[WS_OBJ_FUNCTION] = "FUNCTION",
	[WS_OBJ_INDEX] = "INDEX",
	[WS_OBJ_MATVIEW] = "MATERIALIZED VIEW",
	[WS_OBJ_POLICY] = "POLICY",
	[WS_OBJ_PROCEDURE] = "PROCEDURE",
	[WS_OBJ_RULE] = "RULE",
	[WS_OBJ_SEQUENCE] = "SEQUENCE",
	[WS_OBJ_TRIGGER] = "TRIGGER",
	[WS_OBJ_TYPE] = "TYPE",
	[WS_OBJ_VIEW] = "VIEW",
};

static bool
is_of_operators(WsObjectKind kind)
{
	return kind == WS_OBJ_OPERATOR || kind == WS_OBJ_OPLINK || kind == WS_OBJ_OPCLASS ||
		kind == WS_OBJ_OPFAMILY || kind == WS_OBJ_OPMEMBERS;
}

static bool
is_function(WsObjectKind kind)
{
	return kind == WS_OBJ_FUNCTION || kind == WS_OBJ_PROCEDURE;
}

/* Whether it is of a table, named in what its key says as a function says its types. */
static bool
is_of_table(WsObjectKind kind)
{
	return kind == WS_OBJ_TRIGGER || kind == WS_OBJ_RULE || kind == WS_OBJ_POLICY ||
		kind == WS_OBJ_INDEX || kind == WS_OBJ_DEFAULT;
}

static bool
has_args(WsObjectKind kind)
{
	return is_function(kind) || is_of_table(kind) || kind == WS_OBJ_CAST || is_of_operators(kind);
}

/* Whether its definition is its whole statement, not what follows its name. */
static bool
is_whole_statement(WsObjectKind kind)
{
	return is_of_table(kind) || kind == WS_OBJ_CAST || kind == WS_OBJ_OPLINK || kind == WS_OBJ_OPMEMBERS;
}

bool
ws_obj_before_tables(const WsObject *o)
{
	return o->kind == WS_OBJ_DOMAIN || o->kind == WS_OBJ_TYPE || o->kind == WS_OBJ_SEQUENCE ||
		o->kind == WS_OBJ_COLLATION || o->kind == WS_OBJ_CONVERSION || o->kind == WS_OBJ_CAST ||
		is_of_operators(o->kind) ||
		(is_function(o->kind) && !o->after_tables);
}

static bool
is_relation(WsObjectKind kind)
{
	return kind == WS_OBJ_MATVIEW || kind == WS_OBJ_VIEW;
}

static bool
privileges_problem(const char *p)
{
	if (p == NULL || p[0] == '\0')
		return true;
	for (; *p; p++)
	{
		if (strchr("adDmrtUwxX", *p) == NULL)
			return true;
		if (p[1] != '\0' && (unsigned char) p[0] >= (unsigned char) p[1])
			return true;
	}
	return false;
}

static bool
bad_name(const char *name)
{
	return name == NULL || name[0] == '\0' || strlen(name) >= NAMEDATALEN;
}

/* An object's name or args in this encoding: a cast's are types as printed. */
static char *
name_here(const WsObject *o, const char *name)
{
	return o->kind == WS_OBJ_CAST || o->kind == WS_OBJ_OPERATOR || o->kind == WS_OBJ_OPLINK ?
		ws_text_from_utf8(name) : ws_name_from_utf8(name);
}

/* (order: none first, then by name) */
static int
opt_cmp(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return (a != NULL) - (b != NULL);
	return strcmp(a, b);
}

static bool
key_known(const char *key)
{
	for (int i = 0; i < lengthof(kind_words); i++)
	{
		size_t		n = strlen(kind_words[i]);

		if (strncmp(key, kind_words[i], n) != 0 || key[n] != '/')
			continue;
		if (has_args((WsObjectKind) i))
			return key[n + 1] != '\0' && key[strlen(key) - 1] == ')';
		if (!bad_name(key + n + 1))
			return true;
	}
	return false;
}

const char *
ws_obj_problem(const WsObject *o)
{
	Buf			self = BUF_INIT;
	bool		names_itself = false;

	if (o->kind < 0 || o->kind >= lengthof(kind_words))
		return "an object of no kind";
	/* (a cast's are types as printed, which may be longer than any name) */
	if (o->kind == WS_OBJ_CAST ? (o->name == NULL || o->name[0] == '\0') : bad_name(o->name))
		return "an object of no name, or one too long";
	if (o->definition == NULL || !ws_tags_well_formed(o->definition))
		return "a definition that is not stored text";
	if (o->owner == NULL ? is_relation(o->kind) : bad_name(o->owner) || IsReservedName(o->owner))
		return "no owner, or one no commit holds";
	if (has_args(o->kind) != (o->args != NULL))
		return "a function with no input types, or something else with them";
	if (is_of_table(o->kind) && (o->owner != NULL || o->ngrants > 0 || bad_name(o->args)))
		return "something of a table with no table, or with an owner or grants of its own";
	if (is_of_operators(o->kind) && (o->ngrants > 0 || o->args == NULL || o->args[0] == '\0'))
		return "something of operators with grants, or with no types or method";
	if ((o->kind == WS_OBJ_OPLINK || o->kind == WS_OBJ_OPMEMBERS) && (o->owner != NULL || o->ncomments > 0))
		return "what comes after an operator or family, with an owner or comments of its own";
	if (o->kind == WS_OBJ_CAST && (o->owner != NULL || o->ngrants > 0 || o->args == NULL || o->args[0] == '\0'))
		return "a cast with no target, or with an owner or grants of its own";
	if ((o->after_tables || o->public_revoked) && !is_function(o->kind))
		return "a setting its kind cannot have";
	if (o->fires != 0 && ((o->fires != 'D' && o->fires != 'R' && o->fires != 'A') ||
						  (o->kind != WS_OBJ_TRIGGER && o->kind != WS_OBJ_RULE)))
		return "a state of firing its kind cannot have";
	if ((o->owned_table != NULL) != (o->owned_column != NULL) ||
		(o->owned_table != NULL && (o->kind != WS_OBJ_SEQUENCE || bad_name(o->owned_table) || bad_name(o->owned_column))))
		return "a sequence owned by no column, or something else owned";
	ws_obj_key(o->kind, o->name, o->args, &self);
	buf_append_byte(&self, 0);
	for (int i = 0; i < o->ndeps; i++)
	{
		if (o->deps[i] == NULL || !key_known(o->deps[i]))
			return "a need that is no object's key";
		if (i > 0 && strcmp(o->deps[i - 1], o->deps[i]) >= 0)
			return "needs not in order";
		names_itself |= strcmp(o->deps[i], (char *) self.data) == 0;
	}
	buf_free(&self);
	if (names_itself)
		return "an object that needs itself";
	for (int i = 0; i < o->ncomments; i++)
	{
		const WsObjectComment *c = &o->comments[i];

		if (c->column != NULL && bad_name(c->column))
			return "a comment on a column of no name";
		/* (no comment is empty: making one removes it) */
		if (c->text == NULL || c->text[0] == '\0')
			return "a comment of no text";
		if (i > 0 && opt_cmp(o->comments[i - 1].column, c->column) >= 0)
			return "comments not in order";
	}
	for (int i = 0; i < o->ngrants; i++)
	{
		const WsObjectGrant *g = &o->grants[i];

		if (g->column != NULL && bad_name(g->column))
			return "a grant on a column of no name";
		if (g->grantee != NULL && (bad_name(g->grantee) || IsReservedName(g->grantee)))
			return "a grant to a role no commit holds";
		if (privileges_problem(g->privileges))
			return "a grant of privileges not known, or not in order";
		if (g->grantee != NULL && o->owner != NULL && strcmp(g->grantee, o->owner) == 0)
			return "a grant to its owner";
		if (i > 0)
		{
			const WsObjectGrant *p = &o->grants[i - 1];
			int			c = opt_cmp(p->column, g->column);

			if (c == 0)
				c = opt_cmp(p->grantee, g->grantee);
			if (c == 0)
				c = (int) p->grant_option - (int) g->grant_option;
			if (c >= 0)
				return "grants not in order";
			/* (the one privilege with and without its option is not what a catalog holds) */
			if (opt_cmp(p->column, g->column) == 0 && opt_cmp(p->grantee, g->grantee) == 0)
				for (const char *l = g->privileges; *l; l++)
					if (strchr(p->privileges, *l) != NULL)
						return "a privilege granted twice";
		}
	}
	return NULL;
}

void
ws_obj_key(WsObjectKind kind, const char *name, const char *args, Buf *out)
{
	ws_put_text(out, kind_words[kind]);
	ws_put_text(out, "/");
	ws_put_text(out, name);
	if (args != NULL)
	{
		ws_put_text(out, "(");
		ws_put_text(out, args);
		ws_put_text(out, ")");
	}
}

static void
obj_encode_key(const WsObject *o, Buf *out)
{
	const char *problem = ws_obj_problem(o);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	ws_obj_key(o->kind, o->name, o->args, out);
}

static void
put_string(Buf *out, const char *s)
{
	size_t		n = s != NULL ? strlen(s) : 0;

	buf_append_uvarint(out, n);
	if (n > 0)
		buf_append(out, s, n);
}

static void
obj_encode_value(const WsObject *o, Buf *out)
{
	const char *problem = ws_obj_problem(o);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	put_string(out, o->definition);
	buf_append_byte(out, (o->after_tables ? OBJ_AFTER_TABLES : 0) | (o->public_revoked ? OBJ_PUBLIC_REVOKED : 0) |
					(o->fires == 'D' ? OBJ_FIRES_DISABLED : 0) | (o->fires == 'R' ? OBJ_FIRES_REPLICA : 0) |
					(o->fires == 'A' ? OBJ_FIRES_ALWAYS : 0));
	put_string(out, o->args);
	put_string(out, o->owned_table);
	put_string(out, o->owned_column);
	buf_append_uvarint(out, (uint64) o->ndeps);
	for (int i = 0; i < o->ndeps; i++)
		put_string(out, o->deps[i]);
	put_string(out, o->owner);
	buf_append_uvarint(out, (uint64) o->ncomments);
	for (int i = 0; i < o->ncomments; i++)
	{
		put_string(out, o->comments[i].column);
		put_string(out, o->comments[i].text);
	}
	buf_append_uvarint(out, (uint64) o->ngrants);
	for (int i = 0; i < o->ngrants; i++)
	{
		put_string(out, o->grants[i].column);
		put_string(out, o->grants[i].grantee);
		buf_append_byte(out, o->grants[i].grant_option ? 1 : 0);
		put_string(out, o->grants[i].privileges);
	}
}

pg_noreturn static void
malformed(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a malformed object: %s", what)));
}

static char *
get_string(const uint8 **p, const uint8 *end, bool empty_is_null)
{
	uint64		n;
	char	   *s;

	if (!ws_read_canonical_uvarint(p, end, &n))
		malformed("a number not in its shortest form, or cut short");
	if (n > (uint64) (end - *p) || n > PG_INT32_MAX)
		malformed("a string runs past its end");
	if (n == 0 && empty_is_null)
		return NULL;
	if (memchr(*p, '\0', n) != NULL)
		malformed("a string with a NUL in it");
	if (!pg_verify_mbstr(PG_UTF8, (const char *) *p, (int) n, true))
		malformed("a string not UTF8");
	s = px_malloc(n + 1);
	memcpy(s, *p, n);
	s[n] = '\0';
	*p += n;
	return s;
}

/* A count of things of at least min bytes each, which the bytes left must have. */
static int
get_count(const uint8 **p, const uint8 *end, size_t min)
{
	uint64		n;

	if (!ws_read_canonical_uvarint(p, end, &n))
		malformed("a number not in its shortest form, or cut short");
	if (n > (uint64) (end - *p) / min)
		malformed("more of something than bytes");
	return (int) n;
}

static void *
zeroed(int n, size_t size)
{
	void	   *a = px_malloc((size_t) Max(n, 1) * size);

	memset(a, 0, (size_t) Max(n, 1) * size);
	return a;
}

static void
obj_decode(const uint8 *key, size_t klen, const uint8 *val, size_t vlen, WsObject *o)
{
	const uint8 *p = val;
	const uint8 *end = val + vlen;
	const uint8 *slash = memchr(key, '/', klen);
	const char *problem;
	bool		found = false;
	const uint8 *kp;
	const uint8 *kend = key + klen;
	char	   *name;

	memset(o, 0, sizeof(*o));
	if (slash == NULL)
		malformed("a key of no kind");
	for (int i = 0; i < lengthof(kind_words); i++)
		if (strlen(kind_words[i]) == (size_t) (slash - key) &&
			memcmp(kind_words[i], key, slash - key) == 0)
		{
			o->kind = (WsObjectKind) i;
			found = true;
		}
	if (!found)
		malformed("a key of a kind not known");
	/* (the name is the rest of the key, with no length) */
	kp = slash + 1;
	if (memchr(kp, '\0', kend - kp) != NULL)
		malformed("a string with a NUL in it");
	if (!pg_verify_mbstr(PG_UTF8, (const char *) kp, (int) (kend - kp), true))
		malformed("a string not UTF8");
	name = px_malloc((kend - kp) + 1);
	memcpy(name, kp, kend - kp);
	name[kend - kp] = '\0';
	o->name = name;

	o->definition = get_string(&p, end, false);
	if (p >= end || (*p & ~OBJ_FLAGS) != 0)
		malformed("flags not known");
	o->fires = (*p & OBJ_FIRES_DISABLED) ? 'D' : (*p & OBJ_FIRES_REPLICA) ? 'R' : (*p & OBJ_FIRES_ALWAYS) ? 'A' : 0;
	if (o->fires != 0 && (*p & (OBJ_FIRES_DISABLED | OBJ_FIRES_REPLICA | OBJ_FIRES_ALWAYS)) !=
		(o->fires == 'D' ? OBJ_FIRES_DISABLED : o->fires == 'R' ? OBJ_FIRES_REPLICA : OBJ_FIRES_ALWAYS))
		malformed("two ways of firing");
	o->after_tables = (*p & OBJ_AFTER_TABLES) != 0;
	o->public_revoked = (*p & OBJ_PUBLIC_REVOKED) != 0;
	p++;
	o->args = get_string(&p, end, true);
	o->owned_table = get_string(&p, end, true);
	o->owned_column = get_string(&p, end, true);
	if (has_args(o->kind))
	{
		/* (the name is what is before them, in a key that says them) */
		size_t		nargs = o->args != NULL ? strlen(o->args) : 0;
		size_t		nname = strlen(o->name);

		if (o->args == NULL)
			o->args = px_memdup("", 1);
		if (nname < nargs + 2 || o->name[nname - 1] != ')' || o->name[nname - nargs - 2] != '(' ||
			strncmp(o->name + nname - nargs - 1, o->args, nargs) != 0)
			malformed("a key that does not say a function's input types");
		o->name[nname - nargs - 2] = '\0';
	}
	o->ndeps = get_count(&p, end, 1);
	o->deps = zeroed(o->ndeps, sizeof(char *));
	for (int i = 0; i < o->ndeps; i++)
		o->deps[i] = get_string(&p, end, false);
	o->owner = get_string(&p, end, true);
	o->ncomments = get_count(&p, end, 2);
	o->comments = zeroed(o->ncomments, sizeof(WsObjectComment));
	for (int i = 0; i < o->ncomments; i++)
	{
		o->comments[i].column = get_string(&p, end, true);
		o->comments[i].text = get_string(&p, end, false);
	}
	o->ngrants = get_count(&p, end, 4);
	o->grants = zeroed(o->ngrants, sizeof(WsObjectGrant));
	for (int i = 0; i < o->ngrants; i++)
	{
		WsObjectGrant *g = &o->grants[i];

		g->column = get_string(&p, end, true);
		g->grantee = get_string(&p, end, true);
		if (p >= end || *p > 1)
			malformed("a grant option neither");
		g->grant_option = *p++ == 1;
		g->privileges = get_string(&p, end, false);
	}
	if (p != end)
		malformed("it has bytes left over");
	problem = ws_obj_problem(o);
	if (problem != NULL)
		malformed(problem);
}

const WsObjectCodecRoutine BinaryWsObjectCodec = {
	.name = "binary",
	.encode_key = obj_encode_key,
	.encode_value = obj_encode_value,
	.decode = obj_decode,
};

void
ws_obj_free(WsObject *o)
{
	px_free(o->name);
	px_free(o->args);
	px_free(o->owned_table);
	px_free(o->owned_column);
	px_free(o->definition);
	px_free(o->owner);
	for (int i = 0; i < o->ndeps; i++)
		px_free(o->deps[i]);
	px_free(o->deps);
	for (int i = 0; i < o->ncomments; i++)
	{
		px_free(o->comments[i].column);
		px_free(o->comments[i].text);
	}
	px_free(o->comments);
	for (int i = 0; i < o->ngrants; i++)
	{
		px_free(o->grants[i].column);
		px_free(o->grants[i].grantee);
		px_free(o->grants[i].privileges);
	}
	px_free(o->grants);
	memset(o, 0, sizeof(*o));
}

/* ------------------------------------------------------------------ order */

static int
find_object(const WsObject *objs, int n, const char *key)
{
	int			lo = 0,
				hi = n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		Buf			k = BUF_INIT;
		int			c;

		ws_obj_key(objs[mid].kind, objs[mid].name, objs[mid].args, &k);
		buf_append_byte(&k, 0);
		c = strcmp(key, (char *) k.data);
		buf_free(&k);
		if (c == 0)
			return mid;
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return -1;
}

static int
order_cmp(Datum a, Datum b, void *arg)
{
	return pg_cmp_s32(DatumGetInt32(b), DatumGetInt32(a));
}

/* False if they need each other in a cycle.  An unheld need raises an error. */
static bool
compute_order(const WsObject *objs, int n, int *order)
{
	int		   *remaining = palloc0_array(int, Max(n, 1));
	int		  **needed_by = palloc0_array(int *, Max(n, 1));
	int		   *nneeded_by = palloc0_array(int, Max(n, 1));
	binaryheap *ready = binaryheap_allocate(Max(n, 1), order_cmp, NULL);
	int			done = 0;

	/* (edges from each object's own needs, so bounded by the bytes it took to say so) */
	for (int i = 0; i < n; i++)
		for (int d = 0; d < objs[i].ndeps; d++)
		{
			int			of = find_object(objs, n, objs[i].deps[d]);

			if (of < 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("object \"%s\" needs \"%s\", which a commit does not hold",
								objs[i].name, objs[i].deps[d])));
			remaining[i]++;
			nneeded_by[of]++;
		}
	for (int i = 0; i < n; i++)
	{
		needed_by[i] = palloc_array(int, Max(nneeded_by[i], 1));
		nneeded_by[i] = 0;
	}
	for (int i = 0; i < n; i++)
		for (int d = 0; d < objs[i].ndeps; d++)
		{
			int			of = find_object(objs, n, objs[i].deps[d]);

			needed_by[of][nneeded_by[of]++] = i;
		}

	/* (the ready are taken lowest first, for one order whatever they were found in) */
	for (int i = 0; i < n; i++)
		if (remaining[i] == 0)
			binaryheap_add(ready, Int32GetDatum(i));
	while (!binaryheap_empty(ready))
	{
		int			next = DatumGetInt32(binaryheap_remove_first(ready));

		CHECK_FOR_INTERRUPTS();
		order[done++] = next;
		for (int i = 0; i < nneeded_by[next]; i++)
			if (--remaining[needed_by[next][i]] == 0)
				binaryheap_add(ready, Int32GetDatum(needed_by[next][i]));
	}
	for (int i = 0; i < n; i++)
		pfree(needed_by[i]);
	pfree(needed_by);
	pfree(nneeded_by);
	pfree(remaining);
	binaryheap_free(ready);
	return done == n;
}

/* ---------------------------------------------------------- the catalogs */

/* What no commit can hold of an object in the schema, the first of it. */
static const char *const refusal_sql =
"SELECT c.relname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid "
"      AND d.deptype = 'e') THEN 'it belongs to an extension' "
"  WHEN c.reltablespace <> 0 THEN 'it has a tablespace of its own' "
"  WHEN c.relkind = 'm' AND c.relam <> (SELECT oid FROM pg_am WHERE amname = 'heap') "
"    THEN 'it has another access method' "
"  WHEN EXISTS (SELECT 1 FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid WHERE i.indrelid = c.oid "
"      AND (i.indisclustered OR ic.reltablespace <> 0 OR NOT i.indisvalid OR EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = ic.oid "
"        AND a.attstattarget >= 0))) THEN 'it has an index clustered on, in a tablespace, invalid or with its own statistics' "
"  WHEN EXISTS (SELECT 1 FROM pg_statistic_ext s WHERE s.stxrelid = c.oid) THEN 'it has a statistics object' "
"  WHEN EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 "
"      AND (a.attstattarget >= 0 OR a.attoptions IS NOT NULL)) THEN 'it has settings on a column' "
"  WHEN c.relkind = 'm' AND c.relreplident <> 'd' THEN 'it has a replica identity' "
"  WHEN EXISTS (SELECT 1 FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid WHERE a.attrelid = c.oid "
"      AND a.attnum > 0 AND NOT a.attisdropped AND (a.attstorage <> t.typstorage OR a.attcompression <> '')) "
"    THEN 'it has a column stored its own way' "
"  WHEN EXISTS (SELECT 1 FROM pg_class o WHERE o.oid = c.reltoastrelid AND o.reloptions IS NOT NULL) "
"    THEN 'it has settings on its TOAST table' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid AND d.deptype = 'x') "
"    THEN 'it depends on an extension' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_description m ON m.classoid = 'pg_rewrite'::regclass AND m.objoid = r.oid "
"      WHERE r.ev_class = c.oid AND r.rulename = '_RETURN') THEN 'it has a comment on its rule' "
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = 'pg_class'::regclass AND l.objoid = c.oid) "
"    THEN 'it has a security label' "
"  WHEN c.relacl IS NOT NULL AND (SELECT count(*) FROM aclexplode(c.relacl) e WHERE e.grantee = c.relowner) <> "
"      (SELECT count(*) FROM aclexplode(acldefault('r', c.relowner))) THEN 'its owner''s rights have been changed' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"        JOIN pg_type t ON (d.refclassid = 'pg_type'::regclass AND d.refobjid IN (t.oid, t.typarray) AND t.typrelid = c.oid) "
"      WHERE r.ev_class = c.oid AND r.rulename = '_RETURN' AND d.deptype = 'n') "
"    OR EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"      WHERE r.ev_class = c.oid AND r.rulename = '_RETURN' AND d.deptype = 'n' AND d.refclassid = 'pg_class'::regclass AND d.refobjid = c.oid) "
"    THEN 'it needs itself, as no commit could make it' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"        CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i "
"      WHERE r.ev_class = c.oid AND r.rulename = '_RETURN' AND d.deptype = 'n' AND i.schema IS NOT NULL "
"        AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) "
"    THEN 'it needs something in another schema' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"      WHERE r.ev_class = c.oid AND r.rulename = '_RETURN' AND d.deptype = 'n' AND NOT ( "
"        (d.refclassid = 'pg_class'::regclass AND EXISTS (SELECT 1 FROM pg_class q WHERE q.oid = d.refobjid "
"           AND (q.relkind IN ('r', 'p', 'v', 'm', 'c', 'S') OR q.relnamespace = 'pg_catalog'::regnamespace))) "
"        OR d.refclassid IN ('pg_proc'::regclass, 'pg_collation'::regclass, 'pg_operator'::regclass, 'pg_opclass'::regclass, 'pg_opfamily'::regclass) "
"        OR (d.refclassid = 'pg_constraint'::regclass AND EXISTS (SELECT 1 FROM pg_constraint k "
"           WHERE k.oid = d.refobjid AND k.conrelid <> 0)) "
"        OR d.refclassid = 'pg_type'::regclass "
"        OR coalesce((SELECT i.schema = 'pg_catalog' FROM pg_identify_object(d.refclassid, d.refobjid, 0) i), false))) "
"    THEN 'it needs an object no commit holds yet' "
"  END "
"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
"WHERE n.nspname = $1::name AND c.relkind IN ('v', 'm') ORDER BY 1";

/*
 * (what the active snapshot sees of each relation that a definition or a record would
 * hold, with its owner's and grantees' names: not its row's version, which a TRUNCATE
 * or a VACUUM FULL changes with nothing held changed)
 */
static char *
relations_version(const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;
	char	   *version;

	WsSql->read("SELECT coalesce(string_agg(x, ',' ORDER BY x), '') FROM ( "
				"  SELECT 'c' || c.oid || ':' || c.relname || ':' || c.relkind::text || ':' || coalesce(c.relacl::text, '') || ':' || "
				"      coalesce(c.reloptions::text, '') AS x FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'o' || c.oid || ':' || r.rolname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    JOIN pg_roles r ON r.oid = c.relowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'g' || c.oid || ':' || r.rolname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    CROSS JOIN LATERAL aclexplode(c.relacl) e JOIN pg_roles r ON r.oid = e.grantee WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'h' || a.attrelid || '.' || a.attnum || ':' || r.rolname FROM pg_attribute a "
				"    JOIN pg_class c ON c.oid = a.attrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    CROSS JOIN LATERAL aclexplode(a.attacl) e JOIN pg_roles r ON r.oid = e.grantee WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'a' || a.attrelid || '.' || a.attnum || ':' || a.xmin FROM pg_attribute a "
				"    JOIN pg_class c ON c.oid = a.attrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'r' || r.oid || ':' || r.xmin FROM pg_rewrite r "
				"    JOIN pg_class c ON c.oid = r.ev_class JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || '.' || d.objsubid || ':' || d.description "
				"    FROM pg_description d JOIN pg_class c ON c.oid = d.objoid JOIN pg_namespace n ON n.oid = c.relnamespace "
				"    WHERE d.classoid = 'pg_class'::regclass AND n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description "
				"    FROM pg_description d JOIN pg_type t ON t.oid = d.objoid JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    WHERE d.classoid = 'pg_type'::regclass AND n.nspname = $1::name "
				"  UNION ALL SELECT 't' || t.oid || ':' || t.xmin FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'p' || t.oid || ':' || r.rolname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    JOIN pg_roles r ON r.oid = t.typowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'q' || t.oid || ':' || r.rolname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    CROSS JOIN LATERAL aclexplode(t.typacl) e JOIN pg_roles r ON r.oid = e.grantee WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'f' || p.oid || ':' || p.xmin FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
				"    WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'q' || q.seqrelid || ':' || q.xmin FROM pg_sequence q JOIN pg_class c ON c.oid = q.seqrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'j' || p.oid || ':' || r.rolname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
				"    JOIN pg_roles r ON r.oid = p.proowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'l' || p.oid || ':' || r.rolname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
				"    CROSS JOIN LATERAL aclexplode(p.proacl) e JOIN pg_roles r ON r.oid = e.grantee WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description "
				"    FROM pg_description d JOIN pg_proc p ON p.oid = d.objoid JOIN pg_namespace n ON n.oid = p.pronamespace "
				"    WHERE d.classoid = 'pg_proc'::regclass AND n.nspname = $1::name "
				"  UNION ALL SELECT 'C' || o.oid || ':' || o.xmin || ':' || r.rolname FROM pg_collation o JOIN pg_namespace n ON n.oid = o.collnamespace "
				"    JOIN pg_roles r ON r.oid = o.collowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'V' || o.oid || ':' || o.xmin || ':' || r.rolname FROM pg_conversion o JOIN pg_namespace n ON n.oid = o.connamespace "
				"    JOIN pg_roles r ON r.oid = o.conowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'K' || k.oid || ':' || k.xmin FROM pg_cast k WHERE EXISTS (SELECT 1 FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"      WHERE t.oid IN (k.castsource, k.casttarget) AND n.nspname = $1::name) "
				"    OR EXISTS (SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.oid = k.castfunc AND n.nspname = $1::name) "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_collation o ON d.classoid = 'pg_collation'::regclass AND o.oid = d.objoid JOIN pg_namespace n ON n.oid = o.collnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_conversion o ON d.classoid = 'pg_conversion'::regclass AND o.oid = d.objoid JOIN pg_namespace n ON n.oid = o.connamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_cast k ON d.classoid = 'pg_cast'::regclass AND k.oid = d.objoid JOIN pg_type t ON t.oid IN (k.castsource, k.casttarget) "
				"    JOIN pg_namespace n ON n.oid = t.typnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'O' || o.oid || ':' || o.xmin || ':' || r.rolname FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace "
				"    JOIN pg_roles r ON r.oid = o.oprowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'P' || o.oid || ':' || o.xmin || ':' || r.rolname FROM pg_opclass o JOIN pg_namespace n ON n.oid = o.opcnamespace "
				"    JOIN pg_roles r ON r.oid = o.opcowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'Q' || o.oid || ':' || o.xmin || ':' || r.rolname FROM pg_opfamily o JOIN pg_namespace n ON n.oid = o.opfnamespace "
				"    JOIN pg_roles r ON r.oid = o.opfowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'a' || a.oid || ':' || a.xmin FROM pg_amop a JOIN pg_opfamily o ON o.oid = a.amopfamily JOIN pg_namespace n ON n.oid = o.opfnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'b' || a.oid || ':' || a.xmin FROM pg_amproc a JOIN pg_opfamily o ON o.oid = a.amprocfamily JOIN pg_namespace n ON n.oid = o.opfnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_operator o ON d.classoid = 'pg_operator'::regclass AND o.oid = d.objoid JOIN pg_namespace n ON n.oid = o.oprnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_opclass o ON d.classoid = 'pg_opclass'::regclass AND o.oid = d.objoid JOIN pg_namespace n ON n.oid = o.opcnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_opfamily o ON d.classoid = 'pg_opfamily'::regclass AND o.oid = d.objoid JOIN pg_namespace n ON n.oid = o.opfnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'x' || i.indexrelid || ':' || i.xmin FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'w' || w.oid || ':' || w.xmin FROM pg_attrdef w JOIN pg_class c ON c.oid = w.adrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'g' || g.oid || ':' || g.xmin FROM pg_trigger g JOIN pg_class c ON c.oid = g.tgrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'y' || y.oid || ':' || y.xmin FROM pg_policy y JOIN pg_class c ON c.oid = y.polrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'z' || y.oid || ':' || r.rolname FROM pg_policy y JOIN pg_class c ON c.oid = y.polrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace CROSS JOIN LATERAL unnest(y.polroles) u(o) "
				"    JOIN pg_roles r ON r.oid = u.o WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_trigger g ON d.classoid = 'pg_trigger'::regclass AND g.oid = d.objoid JOIN pg_class c ON c.oid = g.tgrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_policy y ON d.classoid = 'pg_policy'::regclass AND y.oid = d.objoid JOIN pg_class c ON c.oid = y.polrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'd' || d.classoid || '.' || d.objoid || ':' || d.description FROM pg_description d "
				"    JOIN pg_rewrite w ON d.classoid = 'pg_rewrite'::regclass AND w.oid = d.objoid JOIN pg_class c ON c.oid = w.ev_class "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'e' || e.oid || ':' || e.xmin FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid "
				"    JOIN pg_namespace n ON n.oid = t.typnamespace WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'k' || k.oid || ':' || k.xmin FROM pg_constraint k JOIN pg_type t ON t.oid = k.contypid "
				"    JOIN pg_namespace n ON n.oid = t.typnamespace WHERE n.nspname = $1::name) v", 1, params, &rows);
	version = pstrdup(ws_sql_value(&rows, 0, 0));
	ws_sql_rows_free(&rows);
	return version;
}

/*
 * What prints a definition reads its names by the catalog as it is now, whatever
 * snapshot is active.  So read as it is now too: lock each object and what it needs,
 * which keeps them from being renamed, until a new snapshot names nothing more to
 * lock.  That snapshot stays pushed, for the caller to pop.
 *
 * A transaction on an old snapshot reads everything else as of then, so what it would
 * read here could not go with it: raises ERRCODE_T_R_SERIALIZATION_FAILURE if the
 * schema's relations differ from it, to try again.  Gives what they are, as a type or
 * a column's rename takes no lock to wait for: the caller reads again when it is done.
 */
static char *
push_locked_snapshot(const char *schema)
{
	const char *params[1] = {schema};
	HTAB	   *locked;
	char	   *version;
	HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(Oid),.hcxt = CurrentMemoryContext};

	locked = hash_create("objects locked", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (;;)
	{
		WsSqlRows	rows;
		bool		more = false;

		CommandCounterIncrement();
		PushActiveSnapshot(GetLatestSnapshot());
		UpdateActiveSnapshotCommandId();
		WsSql->read("SELECT c.oid::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
					"WHERE n.nspname = $1::name AND c.relkind IN ('v', 'm') "
					"UNION SELECT q.oid::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
					"JOIN pg_rewrite r ON r.ev_class = c.oid "
					"JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
					"LEFT JOIN pg_type t ON d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid "
					"JOIN pg_class q ON q.oid = CASE WHEN d.refclassid = 'pg_class'::regclass THEN d.refobjid ELSE t.typrelid END "
					"WHERE n.nspname = $1::name AND c.relkind IN ('v', 'm')", 1, params, &rows);
		for (int i = 0; i < rows.nrows; i++)
		{
			Oid			oid = atooid(ws_sql_value(&rows, i, 0));
			bool		found;

			(void) hash_search(locked, &oid, HASH_ENTER, &found);
			if (!found)
			{
				LockRelationOid(oid, AccessShareLock);
				more = true;
			}
		}
		ws_sql_rows_free(&rows);
		if (!more)
			break;
		PopActiveSnapshot();
	}
	hash_destroy(locked);
	version = relations_version(schema);
	if (IsolationUsesXactSnapshot())
	{
		char	   *then;

		PushCopiedSnapshot(GetTransactionSnapshot());
		UpdateActiveSnapshotCommandId();
		then = relations_version(schema);
		PopActiveSnapshot();
		if (strcmp(version, then) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not read schema \"%s\": it has changed since this transaction's snapshot", schema)));
		pfree(then);
	}
	return version;
}

/*
 * What a printed query says by a schema's name it says of that schema, which is no
 * name another commit's restore would have: only the catalogs may be named.
 */
typedef struct NamesSchema
{
	const char *why;
	const char *only;			/* this schema's name, or NULL: any but the catalogs' */
} NamesSchema;

static bool
qualifier_counts(const char *qualifier, const char *only)
{
	return only != NULL ? strcmp(qualifier, only) == 0 : strcmp(qualifier, "pg_catalog") != 0;
}

static bool
qualified(List *names, const char *only)
{
	return list_length(names) >= 2 && IsA(linitial(names), String) &&
		qualifier_counts(strVal(linitial(names)), only);
}

/*
 * Whether a name in a string has a qualifier that counts, as the input function of a
 * reg* type will read it at run time: an unquoted part folded to lower case, with
 * spaces around its dot.
 */
static bool
string_is_qualified(const char *s, const char *only)
{
	const char *p = s;

	while (*p)
	{
		StringInfoData part;

		initStringInfo(&part);
		if (*p == '"')
		{
			for (p++; *p; p++)
			{
				if (*p == '"' && p[1] != '"')
				{
					p++;
					break;
				}
				if (*p == '"')
					p++;
				appendStringInfoChar(&part, *p);
			}
		}
		else if (strchr(" (),.", *p) == NULL)
		{
			for (; *p && strchr(" (),.\"", *p) == NULL; p++)
				appendStringInfoChar(&part, pg_tolower((unsigned char) *p));
		}
		else
		{
			pfree(part.data);
			p++;
			continue;
		}
		while (*p == ' ')
			p++;
		if (*p == '.' && qualifier_counts(part.data, only))
		{
			pfree(part.data);
			return true;
		}
		pfree(part.data);
	}
	return false;
}

bool
ws_is_reg_type(const char *type)
{
	static const char *const names[] = {"regclass", "regcollation", "regconfig", "regdatabase", "regdictionary",
		"regnamespace", "regoper", "regoperator", "regproc", "regprocedure", "regrole", "regtype"};

	for (int i = 0; i < lengthof(names); i++)
		if (strcmp(type, names[i]) == 0)
			return true;
	return false;
}

static bool
names_schema_walker(Node *node, void *arg)
{
	NamesSchema *n = arg;
	bool		named = false;

	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_RangeVar:
			named = ((RangeVar *) node)->schemaname != NULL &&
				qualifier_counts(((RangeVar *) node)->schemaname, n->only);
			break;
		case T_TypeName:
			named = qualified(((TypeName *) node)->names, n->only);
			break;
		case T_FuncCall:
			named = qualified(((FuncCall *) node)->funcname, n->only);
			break;
		case T_A_Expr:
			named = qualified(((A_Expr *) node)->name, n->only);
			break;
		case T_CollateClause:
			named = qualified(((CollateClause *) node)->collname, n->only);
			break;
		case T_SortBy:
			named = qualified(((SortBy *) node)->useOp, n->only);
			break;
		case T_TypeCast:
			{
				TypeCast   *c = (TypeCast *) node;
				const char *type = strVal(llast(c->typeName->names));
				Node	   *inner = c->arg;

				if (ws_is_reg_type(type) && c->typeName->arrayBounds != NIL)
				{
					/* (no dependency is held for a name in one, nor is it one name) */
					n->why = "it has an array of reg* names, which say nothing of what they need";
					return true;
				}
				/* (printed with casts around it: as text, and then to regclass) */
				while (IsA(inner, TypeCast))
					inner = ((TypeCast *) inner)->arg;
				if (ws_is_reg_type(type) && IsA(inner, A_Const) &&
					IsA(&((A_Const *) inner)->val, String))
				{
					const char *value = strVal(&((A_Const *) inner)->val);

					named = string_is_qualified(value, n->only) ||
						(strcmp(type, "regnamespace") == 0 &&
						 (n->only != NULL ? (strcmp(value, n->only) == 0 || strcmp(value, quote_identifier(n->only)) == 0) :
						  strcmp(value, "pg_catalog") != 0));
				}
				break;
			}
		case T_ReturnStmt:
			/* (a function's RETURN, which the tree walker has no case for) */
			return names_schema_walker(((ReturnStmt *) node)->returnval, arg);
		case T_RangeSubselect:
			{
				RangeSubselect *r = (RangeSubselect *) node;

				/*
				 * (VALUES with column names prints as a subquery that prints
				 * otherwise once it is one: two sets of bytes for one view)
				 */
				if (n->only == NULL && r->alias != NULL && strcmp(r->alias->aliasname, "*VALUES*") == 0)
				{
					n->why = "it is a VALUES list with column names, which does not print as itself";
					return true;
				}
				break;
			}
		default:
			break;
	}
	if (named)
	{
		n->why = "it must say a schema's name, as what it means has a name something else hides";
		return true;
	}
	return raw_expression_tree_walker(node, names_schema_walker, arg);
}

bool
ws_expression_names_schema(const char *expression, const char *schema)
{
	List	   *parsed = raw_parser(psprintf("SELECT (%s)", expression), RAW_PARSE_DEFAULT);
	NamesSchema n = {NULL, schema};

	return list_length(parsed) == 1 && names_schema_walker(linitial_node(RawStmt, parsed)->stmt, &n);
}

/* Refuses a printed query that would not mean the same in another schema. */
static void
check_query_is_portable(const char *name, const char *schema, const char *query)
{
	List	   *parsed = raw_parser(query, RAW_PARSE_DEFAULT);
	NamesSchema n = {NULL, NULL};

	if (list_length(parsed) != 1)
		elog(ERROR, "a view's definition is not one query");
	if (names_schema_walker(linitial_node(RawStmt, parsed)->stmt, &n))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: %s", name, schema, n.why)));
}

static void
refuse_what_cannot_be_held(const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read(refusal_sql, 1, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
		if (ws_sql_value(&rows, i, 1) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: %s",
							ws_sql_value(&rows, i, 0), schema, ws_sql_value(&rows, i, 1))));
	ws_sql_rows_free(&rows);
}

static char *
role_in_commit(const char *object, const char *role)
{
	char	   *what = psprintf("object \"%s\"", object);

	ws_role_check_name(what, role);
	pfree(what);
	return ws_name_to_utf8(role);
}

/* (none: the object's own) */
static char *
column_in_commit(const char *column)
{
	return column != NULL ? ws_name_to_utf8(column) : NULL;
}

static int
cstr_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

static int
comment_cmp(const void *a, const void *b)
{
	return opt_cmp(((const WsObjectComment *) a)->column, ((const WsObjectComment *) b)->column);
}

static int
grant_cmp(const void *a, const void *b)
{
	const WsObjectGrant *x = a;
	const WsObjectGrant *y = b;
	int			c = opt_cmp(x->column, y->column);

	if (c == 0)
		c = opt_cmp(x->grantee, y->grantee);
	return c != 0 ? c : (int) x->grant_option - (int) y->grant_option;
}

static char *
function_args(const char *schema, Oid oid, char *prokind)
{
	HeapTuple	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(oid));
	Form_pg_proc form;
	char	   *args;

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not read schema \"%s\": it changed while it was being read", schema)));
	form = (Form_pg_proc) GETSTRUCT(tup);
	args = ws_schema_print(schema, oidvectortypes, 1, PointerGetDatum(&form->proargtypes), 0, 0);
	if (prokind != NULL)
		*prokind = form->prokind;
	ReleaseSysCache(tup);
	return args;
}

/* A function's key, as what needs it holds it. */
static void
function_key(const char *schema, Oid oid, Buf *key)
{
	char		prokind;
	char	   *args = ws_text_to_utf8(function_args(schema, oid, &prokind));

	ws_obj_key(prokind == PROKIND_PROCEDURE ? WS_OBJ_PROCEDURE : WS_OBJ_FUNCTION,
			   ws_name_to_utf8(get_func_name(oid)), args, key);
}

static char *print_type(const char *schema, Oid typid, int32 typmod);
static char *printed_or_retry(const char *schema, char *printed);

/* An operator's input types as printed, NONE for what it has not. */
static char *
operator_args(const char *schema, Oid left, Oid right)
{
	return psprintf("%s, %s", OidIsValid(left) ? print_type(schema, left, -1) : "NONE",
					OidIsValid(right) ? print_type(schema, right, -1) : "NONE");
}

static void
operator_key(const char *schema, Oid oid, Buf *key)
{
	const char *params[1];
	WsSqlRows	rows;
	char	   *text = psprintf("%u", oid);

	params[0] = text;
	WsSql->read("SELECT oprname::text, oprleft::text, oprright::text FROM pg_operator WHERE oid = $1::oid", 1, params, &rows);
	if (rows.nrows != 1)
		printed_or_retry(schema, NULL);
	ws_obj_key(WS_OBJ_OPERATOR, ws_name_to_utf8(ws_sql_value(&rows, 0, 0)),
			   ws_text_to_utf8(operator_args(schema, atooid(ws_sql_value(&rows, 0, 1)), atooid(ws_sql_value(&rows, 0, 2)))), key);
	ws_sql_rows_free(&rows);
	pfree(text);
}

/* The key of a class, or of a family, by its name and its method's. */
static void
method_key(bool is_class, Oid oid, Buf *key)
{
	const char *params[1];
	WsSqlRows	rows;
	char	   *text = psprintf("%u", oid);

	params[0] = text;
	WsSql->read(is_class ? "SELECT o.opcname::text, quote_ident(a.amname) FROM pg_opclass o JOIN pg_am a ON a.oid = o.opcmethod WHERE o.oid = $1::oid" :
				"SELECT o.opfname::text, quote_ident(a.amname) FROM pg_opfamily o JOIN pg_am a ON a.oid = o.opfmethod WHERE o.oid = $1::oid",
				1, params, &rows);
	if (rows.nrows != 1)
		elog(ERROR, "no operator class or family %u", oid);
	ws_obj_key(is_class ? WS_OBJ_OPCLASS : WS_OBJ_OPFAMILY, ws_name_to_utf8(ws_sql_value(&rows, 0, 0)),
			   ws_name_to_utf8(ws_sql_value(&rows, 0, 1)), key);
	ws_sql_rows_free(&rows);
	pfree(text);
}

/*
 * What an object needs among a schema's objects: of what depends for it (source, its
 * classid and objid, of $1), the views and the types.  An array is its element's and
 * a multirange its range's, which are what is made.  A table is no need: tables are
 * all made, or a type that needs one is refused.
 */
static void
read_deps_of(WsObject *o, const char *schema, const char *oid, const char *source, const char *self)
{
	const char *params[3] = {oid, schema, self};
	WsSqlRows	rows;
	char	   *sql;

	sql = psprintf("WITH src(classid, objid) AS (%s), "
				   /* (a family's members need what they hold only automatically) */
				   "d AS (SELECT p.refclassid, p.refobjid FROM pg_depend p JOIN src ON p.classid = src.classid "
				   "  AND p.objid = src.objid WHERE p.deptype = 'n' OR "
				   "    (p.deptype = 'a' AND p.classid IN ('pg_amop'::regclass, 'pg_amproc'::regclass))) "
				   "SELECT DISTINCT CASE q.relkind WHEN 'c' THEN 'T' ELSE q.relkind::text END, q.relname::text FROM d "
				   "LEFT JOIN pg_type t ON d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid "
				   "LEFT JOIN pg_type e ON e.oid = t.typelem AND e.typarray = t.oid "
				   "JOIN pg_class q ON q.oid = CASE WHEN d.refclassid = 'pg_class'::regclass THEN d.refobjid "
				   "    ELSE coalesce(e.typrelid, t.typrelid) END "
				   /* (an identity's is made with its table, so is no object to need) */
				   "WHERE q.relkind IN ('v', 'm', 'c', 'S') AND q.oid <> $3::oid "
				   "  AND NOT EXISTS (SELECT 1 FROM pg_depend i WHERE i.classid = 'pg_class'::regclass AND i.objid = q.oid AND i.deptype = 'i' AND i.refclassid = 'pg_class'::regclass) "
				   "  AND q.reltype <> $3::oid AND q.relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2::name) "
				   "UNION SELECT 'L', l.collname::text FROM d JOIN pg_collation l ON d.refclassid = 'pg_collation'::regclass "
				   "  AND l.oid = d.refobjid WHERE l.collnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2::name) "
				   "UNION SELECT 'O', o.oid::text FROM d JOIN pg_operator o ON d.refclassid = 'pg_operator'::regclass "
				   "  AND o.oid = d.refobjid WHERE o.oprnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2::name) "
				   "UNION SELECT 'P', o.oid::text FROM d JOIN pg_opclass o ON d.refclassid = 'pg_opclass'::regclass "
				   "  AND o.oid = d.refobjid WHERE o.opcnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2::name) "
				   "UNION SELECT 'M', o.oid::text FROM d JOIN pg_opfamily o ON d.refclassid = 'pg_opfamily'::regclass "
				   "  AND o.oid = d.refobjid WHERE o.opfnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2::name) "
				   "UNION SELECT 'F', p.oid::text FROM d JOIN pg_proc p ON d.refclassid = 'pg_proc'::regclass AND p.oid = d.refobjid "
				   "WHERE p.oid <> $3::oid AND p.pronamespace <> 'pg_catalog'::regnamespace "
				   "  AND NOT EXISTS (SELECT 1 FROM pg_depend x WHERE x.classid = 'pg_proc'::regclass AND x.objid = p.oid "
				   "    AND x.deptype = 'i' AND x.refclassid = 'pg_type'::regclass) "
				   "UNION SELECT CASE b.typtype WHEN 'd' THEN 'D' ELSE 'T' END, b.typname::text FROM ( "
				   "    SELECT d.refobjid AS typ FROM d WHERE d.refclassid = 'pg_type'::regclass "
				   "    UNION SELECT x.refobjid FROM d JOIN pg_depend x ON d.refclassid = 'pg_proc'::regclass "
				   "      AND x.classid = 'pg_proc'::regclass AND x.objid = d.refobjid AND x.deptype = 'i' "
				   "      AND x.refclassid = 'pg_type'::regclass) n "
				   "JOIN pg_type t ON t.oid = n.typ "
				   "LEFT JOIN pg_type e ON e.oid = t.typelem AND e.typarray = t.oid "
				   "LEFT JOIN pg_range r ON r.rngmultitypid = coalesce(e.oid, t.oid) "
				   "JOIN pg_type b ON b.oid = coalesce(r.rngtypid, e.oid, t.oid) "
				   "LEFT JOIN pg_class c ON c.oid = b.typrelid "
				   "WHERE b.oid <> $3::oid AND (c.oid IS NULL OR c.relkind = 'c') "
				   "  AND b.typnamespace NOT IN ('pg_catalog'::regnamespace)", source);
	WsSql->read(sql, 3, params, &rows);
	pfree(sql);
	o->deps = zeroed(rows.nrows, sizeof(char *));
	o->ndeps = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		Buf			key = BUF_INIT;
		char		k = ws_sql_value(&rows, i, 0)[0];

		if (k == 'F')
			function_key(schema, atooid(ws_sql_value(&rows, i, 1)), &key);
		else if (k == 'O')
			operator_key(schema, atooid(ws_sql_value(&rows, i, 1)), &key);
		else if (k == 'P' || k == 'M')
			method_key(k == 'P', atooid(ws_sql_value(&rows, i, 1)), &key);
		else
			ws_obj_key(k == 'L' ? WS_OBJ_COLLATION : k == 'm' ? WS_OBJ_MATVIEW : k == 'v' ? WS_OBJ_VIEW : k == 'D' ? WS_OBJ_DOMAIN :
					   k == 'S' ? WS_OBJ_SEQUENCE : WS_OBJ_TYPE,
					   ws_name_to_utf8(ws_sql_value(&rows, i, 1)), NULL, &key);
		buf_append_byte(&key, 0);
		o->deps[i] = px_memdup(key.data, key.len);
		buf_free(&key);
	}
	if (o->ndeps > 1)
		qsort(o->deps, o->ndeps, sizeof(char *), cstr_cmp);
	ws_sql_rows_free(&rows);
}

/* What an object needs, itself apart: its oid may be that of a type or function too. */
static void
read_deps(WsObject *o, const char *schema, const char *oid, const char *source)
{
	read_deps_of(o, schema, oid, source, oid);
}

/*
 * Refuses an object whose text may have a constant of a type holding names (of the
 * types a query gives): such a constant is one string no grammar sees into, so it
 * could name the schema there.
 */
static void
check_constants_hold_no_names(const char *name, const char *schema, const char *oid, const char *types)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read(types, 1, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
		if (ws_type_holds_names(atooid(ws_sql_value(&rows, i, 0))))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: a constant of it may hold names no grammar reads",
							name, schema)));
	ws_sql_rows_free(&rows);
}

static void
read_comments(WsObject *o, const char *oid)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT a.attname::text, d.description FROM pg_description d "
				"LEFT JOIN pg_attribute a ON a.attrelid = d.objoid AND a.attnum = d.objsubid AND d.objsubid > 0 "
				"WHERE d.classoid = 'pg_class'::regclass AND d.objoid = $1::oid", 1, params, &rows);
	o->comments = zeroed(rows.nrows, sizeof(WsObjectComment));
	o->ncomments = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		o->comments[i].column = column_in_commit(ws_sql_value(&rows, i, 0));
		o->comments[i].text = ws_text_to_utf8(ws_sql_value(&rows, i, 1));
	}
	if (o->ncomments > 1)
		qsort(o->comments, o->ncomments, sizeof(WsObjectComment), comment_cmp);
	ws_sql_rows_free(&rows);
}

static void
read_owner_and_grants(WsObject *o, const char *oid, const char *name, bool must_travel)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT r.rolname::text, r.rolcanlogin::text, "
				"  (r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)::text "
				"FROM pg_class c JOIN pg_roles r ON r.oid = c.relowner WHERE c.oid = $1::oid", 1, params, &rows);
	/* (it reads with its owner's rights, so another owner could show other rows) */
	if (rows.nrows != 1 || ws_sql_value(&rows, 0, 1)[0] == 't' || ws_sql_value(&rows, 0, 2)[0] == 't' ||
		IsReservedName(ws_sql_value(&rows, 0, 0)))
	{
		if (must_travel)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: it is owned by a role that cannot travel", name),
					 errhint("Make a role that cannot log in and holds no powers its owner, with ALTER ... OWNER TO.")));
	}
	else
		o->owner = role_in_commit(name, ws_sql_value(&rows, 0, 0));
	ws_sql_rows_free(&rows);

	WsSql->read("WITH g(attname, grantee, grantor, owner, is_grantable, privilege_type) AS ( "
				"  SELECT NULL::name, e.grantee, e.grantor, c.relowner, e.is_grantable, e.privilege_type "
				"    FROM pg_class c, aclexplode(c.relacl) e WHERE c.oid = $1::oid "
				"  UNION ALL SELECT a.attname, e.grantee, e.grantor, c.relowner, e.is_grantable, e.privilege_type "
				"    FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid, aclexplode(a.attacl) e "
				"    WHERE c.oid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped) "
				"SELECT g.attname::text, r.rolname::text, g.is_grantable::text, "
				"  bool_or(g.grantor <> g.owner)::text, bool_or(r.rolcanlogin OR r.rolsuper OR r.rolcreaterole "
				"    OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)::text, "
				"  string_agg(CASE g.privilege_type WHEN 'INSERT' THEN 'a' WHEN 'SELECT' THEN 'r' "
				"    WHEN 'UPDATE' THEN 'w' WHEN 'DELETE' THEN 'd' WHEN 'TRUNCATE' THEN 'D' WHEN 'REFERENCES' THEN 'x' "
				"    WHEN 'TRIGGER' THEN 't' WHEN 'MAINTAIN' THEN 'm' WHEN 'USAGE' THEN 'U' END, '') "
				"FROM g LEFT JOIN pg_roles r ON r.oid = g.grantee WHERE g.grantee <> g.owner GROUP BY 1, 2, 3",
				1, params, &rows);
	o->grants = zeroed(rows.nrows, sizeof(WsObjectGrant));
	o->ngrants = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObjectGrant *g = &o->grants[i];
		const char *grantee = ws_sql_value(&rows, i, 1);
		char	   *letters;
		int			n;

		if (ws_sql_value(&rows, i, 3)[0] == 't')
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: it has a grant by a role that does not own it", name)));
		if ((grantee != NULL && IsReservedName(grantee)) ||
			(ws_sql_value(&rows, i, 4) != NULL && ws_sql_value(&rows, i, 4)[0] == 't'))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: it has a grant to role \"%s\", which cannot travel",
							name, grantee)));
		g->column = column_in_commit(ws_sql_value(&rows, i, 0));
		g->grantee = grantee != NULL ? role_in_commit(name, grantee) : NULL;
		g->grant_option = ws_sql_value(&rows, i, 2)[0] == 't';
		letters = px_memdup(ws_sql_value(&rows, i, 5), strlen(ws_sql_value(&rows, i, 5)) + 1);
		n = (int) strlen(letters);
		for (int a = 1; a < n; a++)
			for (int b = a; b > 0 && (unsigned char) letters[b - 1] > (unsigned char) letters[b]; b--)
			{
				char		swap = letters[b];

				letters[b] = letters[b - 1];
				letters[b - 1] = swap;
			}
		g->privileges = letters;
	}
	if (o->ngrants > 1)
		qsort(o->grants, o->ngrants, sizeof(WsObjectGrant), grant_cmp);
	ws_sql_rows_free(&rows);
}

static int
object_cmp(const void *a, const void *b)
{
	const WsObject *x = a;
	const WsObject *y = b;
	Buf			kx = BUF_INIT;
	Buf			ky = BUF_INIT;
	int			c;

	ws_obj_key(x->kind, x->name, x->args, &kx);
	buf_append_byte(&kx, 0);
	ws_obj_key(y->kind, y->name, y->args, &ky);
	buf_append_byte(&ky, 0);
	c = strcmp((char *) kx.data, (char *) ky.data);
	buf_free(&kx);
	buf_free(&ky);
	return c;
}

/* ---------------------------------------------------------------- types */

/* The types of a schema that are objects of their own. */
#define TYPES_OF_SCHEMA \
	"FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace " \
	"LEFT JOIN pg_class c ON c.oid = t.typrelid " \
	"WHERE n.nspname = $1::name AND (c.oid IS NULL OR c.relkind = 'c') AND t.typtype <> 'm' " \
	"AND NOT EXISTS (SELECT 1 FROM pg_type a WHERE a.typarray = t.oid) "

/* What depends for a type, of an expression for its oid. */
#define TYPE_SOURCE_OF(oid) \
	"SELECT 'pg_type'::regclass::oid, u.oid FROM pg_type u WHERE u.oid = " oid " " \
	"UNION ALL SELECT 'pg_class'::regclass::oid, u.typrelid FROM pg_type u WHERE u.oid = " oid " AND u.typrelid <> 0 " \
	"UNION ALL SELECT 'pg_constraint'::regclass::oid, u.oid FROM pg_constraint u WHERE u.contypid = " oid

static const char *const type_refusal_sql =
"SELECT t.typname::text, CASE "
"  WHEN t.typtype IN ('b', 'p') THEN 'it is a base type, or one with no definition yet' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_type'::regclass AND d.objid = t.oid AND d.deptype = 'e') "
"    THEN 'it belongs to an extension' "
"  WHEN EXISTS (SELECT 1 FROM pg_constraint k WHERE k.contypid = t.oid AND NOT k.convalidated) "
"    THEN 'it has a constraint not validated, which no statement could make' "
"  WHEN EXISTS (SELECT 1 FROM pg_constraint k JOIN pg_description m ON m.classoid = 'pg_constraint'::regclass AND m.objoid = k.oid "
"      WHERE k.contypid = t.oid) THEN 'it has a comment on a constraint' "
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE (l.classoid = 'pg_type'::regclass AND l.objoid IN (t.oid, t.typarray, "
"        (SELECT r.rngmultitypid FROM pg_range r WHERE r.rngtypid = t.oid), "
"        (SELECT m.typarray FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid WHERE r.rngtypid = t.oid))) "
"      OR (l.classoid = 'pg_class'::regclass AND l.objoid = t.typrelid AND t.typrelid <> 0)) "
"    THEN 'it has a security label' "
"  WHEN EXISTS (SELECT 1 FROM pg_description x WHERE x.classoid = 'pg_type'::regclass AND x.objoid IN (t.typarray, "
"        (SELECT m.typarray FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid WHERE r.rngtypid = t.oid))) "
"    THEN 'its array type has a comment of its own' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d JOIN pg_proc p ON p.oid = d.objid WHERE d.classid = 'pg_proc'::regclass AND d.deptype = 'i' "
"      AND d.refclassid = 'pg_type'::regclass AND d.refobjid IN (t.oid, (SELECT r.rngmultitypid FROM pg_range r WHERE r.rngtypid = t.oid)) "
"      AND ((p.proacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(p.proacl) e WHERE e.grantee = 0 AND e.privilege_type = 'EXECUTE')) "
"        OR EXISTS (SELECT 1 FROM pg_description x WHERE x.classoid = 'pg_proc'::regclass AND x.objoid = p.oid) "
"        OR EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = 'pg_proc'::regclass AND l.objoid = p.oid) "
"        OR p.pronamespace <> t.typnamespace OR p.proowner <> 10::oid OR p.proconfig IS NOT NULL "
"        OR p.provolatile <> 'i' OR p.prosecdef "
"        OR p.proname NOT IN (t.typname, (SELECT m.typname FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid "
"          WHERE r.rngtypid = t.oid)))) "
"    THEN 'a function of it has been changed from what making it makes, or has a comment or grant of its own' "
"  WHEN t.typtype = 'r' AND EXISTS (SELECT 1 FROM pg_proc c WHERE c.pronamespace = 'pg_catalog'::regnamespace AND c.proname IN (t.typname, "
"      (SELECT m.typname FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid WHERE r.rngtypid = t.oid))) "
"    THEN 'it would have a function named as one in the catalogs' "
"  WHEN EXISTS (SELECT 1 FROM aclexplode(t.typacl) e WHERE e.grantor <> t.typowner) "
"    THEN 'it has a grant by a role that does not own it' "
"  WHEN t.typacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(t.typacl) e WHERE e.grantee = 0) "
"    THEN 'its USAGE has been revoked from PUBLIC' "
"  WHEN t.typacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(t.typacl) e WHERE e.grantee = t.typowner) "
"    THEN 'its owner''s rights have been changed' "
"  WHEN EXISTS (SELECT 1 FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid "
"      WHERE r.rngtypid = t.oid AND (m.typacl IS NOT NULL "
"        OR EXISTS (SELECT 1 FROM pg_description x WHERE x.classoid = 'pg_type'::regclass AND x.objoid = m.oid))) "
"    THEN 'its multirange type has a comment or grant of its own' "
"  WHEN EXISTS (SELECT 1 FROM pg_attribute a WHERE c.oid IS NOT NULL AND a.attrelid = c.oid AND a.attnum > 0 "
"      AND NOT a.attisdropped AND a.attstattarget >= 0) THEN 'it has settings on a column' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"        CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i "
"      WHERE d.deptype = 'n' AND i.schema IS NOT NULL AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) "
"    THEN 'it needs something in another schema' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"      WHERE d.deptype = 'n' AND d.refclassid = 'pg_type'::regclass AND EXISTS (SELECT 1 FROM pg_type y "
"        LEFT JOIN pg_type e ON e.oid = y.typelem AND e.typarray = y.oid "
"        JOIN pg_class q ON q.oid = coalesce(e.typrelid, y.typrelid) "
"        WHERE y.oid = d.refobjid AND q.relkind <> 'c')) "
"    THEN 'it needs a table''s or a view''s row type, and a table may need it' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"      WHERE d.deptype = 'n' AND d.refclassid = 'pg_type'::regclass AND d.refobjid IN (t.oid, t.typarray)) "
"    THEN 'it needs itself, as no commit could make it' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"      WHERE d.deptype = 'n' AND d.refclassid <> 'pg_type'::regclass "
"        AND NOT (d.refclassid = 'pg_namespace'::regclass AND d.refobjid = n.oid AND s.classid <> 'pg_constraint'::regclass) "
"        AND NOT (d.refclassid = 'pg_class'::regclass AND EXISTS (SELECT 1 FROM pg_class q WHERE q.oid = d.refobjid AND q.relkind IN ('c', 'S') "
"          AND NOT EXISTS (SELECT 1 FROM pg_depend i WHERE i.classid = 'pg_class'::regclass AND i.objid = q.oid AND i.deptype = 'i' AND i.refclassid = 'pg_class'::regclass))) "
"        AND d.refclassid NOT IN ('pg_proc'::regclass, 'pg_collation'::regclass, 'pg_operator'::regclass, 'pg_opclass'::regclass, 'pg_opfamily'::regclass) "
"        AND NOT coalesce((SELECT i.schema = 'pg_catalog' "
"        FROM pg_identify_object(d.refclassid, d.refobjid, 0) i), false)) "
"    THEN 'it needs an object no commit holds yet' "
"  END " TYPES_OF_SCHEMA "ORDER BY 1";

/* The casts of a schema: of a type or function in it; not a range's own. */
#define CASTS_OF_SCHEMA \
	"FROM pg_cast k, pg_namespace n WHERE n.nspname = $1::name AND NOT EXISTS (SELECT 1 FROM pg_depend d " \
	"    WHERE d.classid = 'pg_cast'::regclass AND d.objid = k.oid AND d.deptype = 'i') " \
	"  AND (EXISTS (SELECT 1 FROM pg_type t WHERE t.oid IN (k.castsource, k.casttarget) AND t.typnamespace = n.oid) " \
	"    OR EXISTS (SELECT 1 FROM pg_proc p WHERE p.oid = k.castfunc AND p.pronamespace = n.oid)) "

static const char *const cast_refusal_sql =
"SELECT format('(%s AS %s)', k.castsource::regtype, k.casttarget::regtype), CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i "
"      WHERE d.classid = 'pg_cast'::regclass AND d.objid = k.oid AND d.deptype = 'n' AND i.schema IS NOT NULL "
"        AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) THEN 'it needs something in another schema' "
"  WHEN EXISTS (SELECT 1 FROM pg_type t LEFT JOIN pg_type e ON e.oid = t.typelem AND e.typarray = t.oid "
"      JOIN pg_class q ON q.oid = coalesce(e.typrelid, t.typrelid) WHERE t.oid IN (k.castsource, k.casttarget) AND q.relkind <> 'c') "
"    THEN 'it is of a table''s or a view''s row type, and a table may need it' "
"  WHEN NOT EXISTS (SELECT 1 FROM pg_type t WHERE t.oid IN (k.castsource, k.casttarget) AND t.typnamespace = n.oid) "
"    THEN 'it is between types in the catalogs, which where it arrives may already be cast' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_cast'::regclass AND d.objid = k.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension' "
"  END " CASTS_OF_SCHEMA "ORDER BY 1";

/* What comes with a range cannot be said apart from it, nor can its comment. */
static const char *const range_cast_refusal_sql =
"SELECT format('(%s AS %s)', k.castsource::regtype, k.casttarget::regtype), "
"  'it is a range''s own, with a comment, which only what comes with the range could hold' "
"FROM pg_cast k JOIN pg_description m ON m.classoid = 'pg_cast'::regclass AND m.objoid = k.oid "
"JOIN pg_depend d ON d.classid = 'pg_cast'::regclass AND d.objid = k.oid AND d.deptype = 'i' AND d.refclassid = 'pg_type'::regclass "
"JOIN pg_type t ON t.oid = d.refobjid JOIN pg_namespace n ON n.oid = t.typnamespace WHERE n.nspname = $1::name ORDER BY 1";

static void
refuse_types(const char *schema, const char *sql)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read(sql, 1, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
		if (ws_sql_value(&rows, i, 1) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: %s",
							ws_sql_value(&rows, i, 0), schema, ws_sql_value(&rows, i, 1))));
	ws_sql_rows_free(&rows);
}

/* Refuses a qualified name in a statement generated for a type. */
static void
check_type_is_portable(const char *name, const char *schema, const char *head, const char *tail)
{
	char	   *sql = psprintf("%s x %s", head, tail);
	List	   *parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	Node	   *node = linitial_node(RawStmt, parsed)->stmt;
	NamesSchema n = {NULL, NULL};
	bool		named = false;
	ListCell   *lc;

	if (IsA(node, CreateDomainStmt))
	{
		CreateDomainStmt *d = (CreateDomainStmt *) node;

		named = names_schema_walker((Node *) d->typeName, &n) ||
			names_schema_walker((Node *) d->collClause, &n);
		foreach(lc, d->constraints)
			named = named || names_schema_walker(lfirst_node(Constraint, lc)->raw_expr, &n);
	}
	else if (IsA(node, CompositeTypeStmt))
	{
		foreach(lc, ((CompositeTypeStmt *) node)->coldeflist)
		{
			ColumnDef  *c = lfirst_node(ColumnDef, lc);

			named = named || names_schema_walker((Node *) c->typeName, &n) ||
				names_schema_walker((Node *) c->collClause, &n);
		}
	}
	else if (IsA(node, CreateRangeStmt))
	{
		foreach(lc, ((CreateRangeStmt *) node)->params)
		{
			Node	   *arg = lfirst_node(DefElem, lc)->arg;

			if (IsA(arg, TypeName))
				named = named || names_schema_walker(arg, &n);
			else if (IsA(arg, List) && qualified((List *) arg, n.only))
			{
				n.why = "it must say a schema's name, having something named as the catalogs' is, or from a schema renamed under it";
				named = true;
			}
		}
	}
	if (named)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: %s", name, schema, n.why)));
	pfree(sql);
}

/*
 * What a printing function gives, which is nothing for what was dropped since this
 * read's snapshot: to try again, as what a read overtaken ends with anyway.
 */
static char *
printed_or_retry(const char *schema, char *printed)
{
	if (printed == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not read schema \"%s\": it changed while it was being read", schema)));
	return printed;
}

/*
 * A type as the statement says it, printed under the schema's path, as nothing else
 * here runs: what is in the schema is not named.
 */
static char *
print_type(const char *schema, Oid typid, int32 typmod)
{
	return printed_or_retry(schema, ws_schema_print(schema, format_type, 2, ObjectIdGetDatum(typid),
													Int32GetDatum(typmod), 0));
}

/* The statement after its name of a type of each kind, into text. */
static void
type_definition(const char *schema, char typtype, const char *oid, Buf *text)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	switch (typtype)
	{
		case 'e':
			WsSql->read("SELECT coalesce(string_agg(quote_literal(enumlabel), ', ' ORDER BY enumsortorder), '') "
						"FROM pg_enum WHERE enumtypid = $1::oid", 1, params, &rows);
			ws_put_text(text, "AS ENUM (");
			ws_put_text(text, ws_sql_value(&rows, 0, 0));
			ws_put_text(text, ")");
			break;
		case 'c':
			WsSql->read("SELECT quote_ident(a.attname), a.atttypid::text, a.atttypmod::text, "
						"  CASE WHEN a.attcollation <> u.typcollation THEN quote_ident(k.collname) END "
						"FROM pg_type t JOIN pg_attribute a ON a.attrelid = t.typrelid "
						"JOIN pg_type u ON u.oid = a.atttypid LEFT JOIN pg_collation k ON k.oid = a.attcollation "
						"WHERE t.oid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum",
						1, params, &rows);
			ws_put_text(text, "AS (");
			for (int i = 0; i < rows.nrows; i++)
			{
				if (i > 0)
					ws_put_text(text, ", ");
				ws_put_text(text, ws_sql_value(&rows, i, 0));
				ws_put_text(text, " ");
				ws_put_text(text, print_type(schema, atooid(ws_sql_value(&rows, i, 1)),
											 atoi(ws_sql_value(&rows, i, 2))));
				if (ws_sql_value(&rows, i, 3) != NULL)
				{
					ws_put_text(text, " COLLATE ");
					ws_put_text(text, ws_sql_value(&rows, i, 3));
				}
			}
			ws_put_text(text, ")");
			break;
		case 'r':
			WsSql->read("SELECT r.rngsubtype::text, "
						"  CASE WHEN o.opcdefault THEN '' ELSE ', SUBTYPE_OPCLASS = ' || quote_ident(o.opcname) END || "
						"  CASE WHEN r.rngcollation <> s.typcollation "
						"    THEN ', COLLATION = ' || quote_ident(k.collname) ELSE '' END || "
						"  CASE WHEN r.rngcanonical <> 0 THEN ', CANONICAL = ' || r.rngcanonical::regproc ELSE '' END || "
						/* (one of its schema's is said bare: regproc here would say the schema) */
						"  CASE WHEN r.rngsubdiff = 0 THEN '' "
						"    WHEN d.pronamespace = (SELECT typnamespace FROM pg_type WHERE oid = r.rngtypid) "
						"    THEN ', SUBTYPE_DIFF = ' || quote_ident(d.proname) "
						"    ELSE ', SUBTYPE_DIFF = ' || r.rngsubdiff::regproc END || ')' "
						"FROM pg_range r JOIN pg_type s ON s.oid = r.rngsubtype JOIN pg_opclass o ON o.oid = r.rngsubopc "
						"LEFT JOIN pg_proc d ON d.oid = r.rngsubdiff "
						"LEFT JOIN pg_collation k ON k.oid = r.rngcollation WHERE r.rngtypid = $1::oid", 1, params, &rows);
			ws_put_text(text, "AS RANGE (SUBTYPE = ");
			ws_put_text(text, print_type(schema, atooid(ws_sql_value(&rows, 0, 0)), -1));
			ws_put_text(text, ws_sql_value(&rows, 0, 1));
			break;
		case 'd':
			{
				WsSqlRows	cons;

				WsSql->read("SELECT t.typbasetype::text, t.typtypmod::text, "
							"  CASE WHEN t.typcollation <> b.typcollation THEN quote_ident(k.collname) END, "
							"  t.typdefaultbin::text "
							"FROM pg_type t JOIN pg_type b ON b.oid = t.typbasetype "
							"LEFT JOIN pg_collation k ON k.oid = t.typcollation WHERE t.oid = $1::oid", 1, params, &rows);
				ws_put_text(text, "AS ");
				ws_put_text(text, print_type(schema, atooid(ws_sql_value(&rows, 0, 0)),
											 atoi(ws_sql_value(&rows, 0, 1))));
				if (ws_sql_value(&rows, 0, 2) != NULL)
				{
					ws_put_text(text, " COLLATE ");
					ws_put_text(text, ws_sql_value(&rows, 0, 2));
				}
				if (ws_sql_value(&rows, 0, 3) != NULL)
				{
					ws_put_text(text, " DEFAULT ");
					ws_put_text(text, printed_or_retry(schema, ws_schema_print(schema, pg_get_expr, 2,
																			   CStringGetTextDatum(ws_sql_value(&rows, 0, 3)),
																			   ObjectIdGetDatum(InvalidOid), 0)));
				}
				WsSql->read("SELECT quote_ident(conname), oid::text FROM pg_constraint WHERE contypid = $1::oid "
							"ORDER BY conname", 1, params, &cons);
				for (int i = 0; i < cons.nrows; i++)
				{
					ws_put_text(text, " CONSTRAINT ");
					ws_put_text(text, ws_sql_value(&cons, i, 0));
					ws_put_text(text, " ");
					INJECTION_POINT("ws-obj-print-constraint", NULL);
					ws_put_text(text, printed_or_retry(schema, ws_schema_print(schema, pg_get_constraintdef, 1,
																			   ObjectIdGetDatum(atooid(ws_sql_value(&cons, i, 1))), 0, 0)));
				}
				ws_sql_rows_free(&cons);
				break;
			}
		default:
			elog(ERROR, "a type of no kind held: %c", typtype);
	}
	ws_sql_rows_free(&rows);
}

static void
read_type_comments(WsObject *o, const char *oid)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT NULL::text, d.description FROM pg_description d "
				"  WHERE d.classoid = 'pg_type'::regclass AND d.objoid = $1::oid "
				"UNION ALL SELECT a.attname::text, d.description FROM pg_type t "
				"  JOIN pg_description d ON d.classoid = 'pg_class'::regclass AND d.objoid = t.typrelid AND d.objsubid > 0 "
				"  JOIN pg_attribute a ON a.attrelid = t.typrelid AND a.attnum = d.objsubid "
				"  WHERE t.oid = $1::oid", 1, params, &rows);
	o->comments = zeroed(rows.nrows, sizeof(WsObjectComment));
	o->ncomments = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		o->comments[i].column = column_in_commit(ws_sql_value(&rows, i, 0));
		o->comments[i].text = ws_text_to_utf8(ws_sql_value(&rows, i, 1));
	}
	if (o->ncomments > 1)
		qsort(o->comments, o->ncomments, sizeof(WsObjectComment), comment_cmp);
	ws_sql_rows_free(&rows);
}

/* Its owner, if one that can travel, and what it has granted beyond PUBLIC's USAGE. */
static void
read_type_owner_and_grants(WsObject *o, const char *oid, const char *name)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT r.rolname::text FROM pg_type t JOIN pg_roles r ON r.oid = t.typowner "
				"WHERE t.oid = $1::oid AND NOT r.rolcanlogin AND NOT (r.rolsuper OR r.rolcreaterole "
				"  OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)", 1, params, &rows);
	if (rows.nrows == 1 && !IsReservedName(ws_sql_value(&rows, 0, 0)))
		o->owner = role_in_commit(name, ws_sql_value(&rows, 0, 0));
	ws_sql_rows_free(&rows);

	WsSql->read("SELECT r.rolname::text, e.is_grantable::text, (e.grantor <> t.typowner)::text, "
				"  (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)::text "
				"FROM pg_type t, aclexplode(t.typacl) e JOIN pg_roles r ON r.oid = e.grantee "
				"WHERE t.oid = $1::oid AND e.grantee <> t.typowner", 1, params, &rows);
	o->grants = zeroed(rows.nrows, sizeof(WsObjectGrant));
	o->ngrants = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		const char *grantee = ws_sql_value(&rows, i, 0);

		if (ws_sql_value(&rows, i, 2)[0] == 't')
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: it has a grant by a role that does not own it", name)));
		if (IsReservedName(grantee) || ws_sql_value(&rows, i, 3)[0] == 't')
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: it has a grant to role \"%s\", which cannot travel",
							name, grantee)));
		o->grants[i].grantee = role_in_commit(name, grantee);
		o->grants[i].grant_option = ws_sql_value(&rows, i, 1)[0] == 't';
		o->grants[i].privileges = px_memdup("U", 2);
	}
	if (o->ngrants > 1)
		qsort(o->grants, o->ngrants, sizeof(WsObjectGrant), grant_cmp);
	ws_sql_rows_free(&rows);
}

/*
 * Refuses what the statement could not say of a type: a multirange not named as
 * CREATE TYPE would name it, and a name too long for a commit.
 */
static void
check_type_names(const char *oid, const char *name, char typtype)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	if (typtype == 'r')
	{
		const char *range = strstr(name, "range");
		char	   *expected;

		if (range != NULL)
			expected = psprintf("%s%s%s", pnstrdup(name, range - name), "multi", range);
		else
			expected = psprintf("%s_multirange", pnstrdup(name, NAMEDATALEN - 12));
		expected[pg_mbcliplen(expected, strlen(expected), NAMEDATALEN - 1)] = '\0';
		WsSql->read("SELECT m.typname::text FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid "
					"WHERE r.rngtypid = $1::oid", 1, params, &rows);
		if (rows.nrows != 1 || strcmp(ws_sql_value(&rows, 0, 0), expected) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: its multirange type has a name of its own", name)));
		ws_sql_rows_free(&rows);
		pfree(expected);
	}
	WsSql->read("SELECT a.attname::text FROM pg_type t JOIN pg_attribute a ON a.attrelid = t.typrelid "
				"  WHERE t.oid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
				"UNION ALL SELECT c.conname::text FROM pg_constraint c WHERE c.contypid = $1::oid", 1, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
		pfree(ws_name_to_utf8(ws_sql_value(&rows, i, 0)));
	ws_sql_rows_free(&rows);
}

/* Adds the types of a schema to objs, of *n and room for it. */
static WsObject *
read_types(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	types;

	WsSql->read("SELECT t.oid::text, t.typname::text, t.typtype::text " TYPES_OF_SCHEMA, 1, params, &types);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + types.nrows, 1));
	for (int i = 0; i < types.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&types, i, 0);
		const char *name = ws_sql_value(&types, i, 1);
		char		typtype = ws_sql_value(&types, i, 2)[0];
		Buf			text = BUF_INIT;
		Buf			stored = BUF_INIT;

		CHECK_FOR_INTERRUPTS();
		o->kind = typtype == 'd' ? WS_OBJ_DOMAIN : WS_OBJ_TYPE;
		o->name = ws_name_to_utf8(name);
		check_type_names(oid, name, typtype);
		type_definition(schema, typtype, oid, &text);
		buf_append_byte(&text, 0);
		check_type_is_portable(name, schema, typtype == 'd' ? "CREATE DOMAIN" : "CREATE TYPE", (char *) text.data);
		ws_tags_escape(ws_text_to_utf8((char *) text.data), &stored);
		buf_append_byte(&stored, 0);
		o->definition = px_memdup(stored.data, stored.len);
		buf_free(&stored);
		buf_free(&text);
		if (typtype == 'd')
			check_constants_hold_no_names(name, schema, oid,
										  "SELECT typbasetype::text FROM pg_type WHERE oid = $1::oid "
										  "  AND (typdefaultbin IS NOT NULL OR EXISTS (SELECT 1 FROM pg_constraint WHERE contypid = $1::oid AND contype = 'c')) "
										  "UNION SELECT d.refobjid::text FROM pg_constraint c JOIN pg_depend d ON d.classid = 'pg_constraint'::regclass "
										  "  AND d.objid = c.oid WHERE c.contypid = $1::oid AND d.deptype = 'n' AND d.refclassid = 'pg_type'::regclass");
		read_deps(o, schema, oid, TYPE_SOURCE_OF("$1::oid"));
		read_type_comments(o, oid);
		read_type_owner_and_grants(o, oid, name);
	}
	ws_sql_rows_free(&types);
	return objs;
}

/* ------------------------------------------------------------ sequences */

/* A schema's own sequences: not an identity's, which is its table's. */
#define SEQUENCES_OF_SCHEMA \
	"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_sequence q ON q.seqrelid = c.oid " \
	"WHERE n.nspname = $1::name AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_class'::regclass " \
	"  AND d.objid = c.oid AND d.deptype = 'i') "

static const char *const sequence_refusal_sql =
"SELECT c.relname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid AND d.deptype = 'e') "
"    THEN 'it belongs to an extension' "
"  WHEN c.relpersistence <> 'p' THEN 'it is not logged' "
"  WHEN EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attacl IS NOT NULL) "
"    THEN 'it has a grant on a column' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d JOIN pg_class t ON t.oid = d.refobjid WHERE d.classid = 'pg_class'::regclass "
"      AND d.objid = c.oid AND d.deptype = 'a' AND d.refclassid = 'pg_class'::regclass AND t.relkind NOT IN ('r', 'p', 'v')) "
"    THEN 'it is owned by a column of what no commit holds' "
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = 'pg_class'::regclass AND l.objoid = c.oid) "
"    THEN 'it has a security label' "
"  WHEN c.relacl IS NOT NULL AND (SELECT count(*) FROM aclexplode(c.relacl) e WHERE e.grantee = c.relowner) <> "
"      (SELECT count(*) FROM aclexplode(acldefault('s', c.relowner))) THEN 'its owner''s rights have been changed' "
"  END " SEQUENCES_OF_SCHEMA "ORDER BY 1";

/* Adds the sequences of a schema to objs, of *n and room for it. */
static WsObject *
read_sequences(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	seqs;

	WsSql->read("SELECT c.oid::text, c.relname::text, q.seqtypid::text, q.seqincrement::text, q.seqmin::text, "
				"  q.seqmax::text, q.seqstart::text, q.seqcache::text, q.seqcycle::text, "
				"  t.relname::text, a.attname::text "
				"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_sequence q ON q.seqrelid = c.oid "
				"LEFT JOIN pg_depend d ON d.classid = 'pg_class'::regclass AND d.objid = c.oid AND d.deptype = 'a' "
				"  AND d.refclassid = 'pg_class'::regclass "
				"LEFT JOIN pg_class t ON t.oid = d.refobjid "
				"LEFT JOIN pg_attribute a ON a.attrelid = d.refobjid AND a.attnum = d.refobjsubid "
				"WHERE n.nspname = $1::name AND NOT EXISTS (SELECT 1 FROM pg_depend i WHERE i.classid = 'pg_class'::regclass "
				"  AND i.objid = c.oid AND i.deptype = 'i')", 1, params, &seqs);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + seqs.nrows, 1));
	for (int i = 0; i < seqs.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&seqs, i, 0);
		const char *name = ws_sql_value(&seqs, i, 1);
		char	   *text;
		Buf			stored = BUF_INIT;

		CHECK_FOR_INTERRUPTS();
		o->kind = WS_OBJ_SEQUENCE;
		o->name = ws_name_to_utf8(name);
		text = psprintf("AS %s INCREMENT BY %s MINVALUE %s MAXVALUE %s START WITH %s CACHE %s %sCYCLE",
						print_type(schema, atooid(ws_sql_value(&seqs, i, 2)), -1),
						ws_sql_value(&seqs, i, 3), ws_sql_value(&seqs, i, 4), ws_sql_value(&seqs, i, 5),
						ws_sql_value(&seqs, i, 6), ws_sql_value(&seqs, i, 7),
						ws_sql_value(&seqs, i, 8)[0] == 't' ? "" : "NO ");
		ws_tags_escape(text, &stored);
		buf_append_byte(&stored, 0);
		o->definition = px_memdup(stored.data, stored.len);
		buf_free(&stored);
		if (ws_sql_value(&seqs, i, 9) != NULL)
		{
			o->owned_table = ws_name_to_utf8(ws_sql_value(&seqs, i, 9));
			o->owned_column = ws_name_to_utf8(ws_sql_value(&seqs, i, 10));
		}
		o->deps = zeroed(0, sizeof(char *));
		read_comments(o, oid);
		read_owner_and_grants(o, oid, name, false);
	}
	ws_sql_rows_free(&seqs);
	return objs;
}

static int
position_cmp(const void *a, const void *b)
{
	return strcmp(((const WsSeqPosition *) a)->name, ((const WsSeqPosition *) b)->name);
}

void
ws_seq_read(const char *schema, WsSeqPosition **out, int *n)
{
	int			level = WsSql->begin();
	const char *params[1] = {schema};
	WsSqlRows	seqs;

	ws_schema_check_usage(schema);
	WsSql->run("SELECT c.relname::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
			   "WHERE n.nspname = $1::name AND c.relkind = 'S'", 1, params, &seqs);
	*out = palloc0_array(WsSeqPosition, Max(seqs.nrows, 1));
	*n = seqs.nrows;
	for (int i = 0; i < seqs.nrows; i++)
	{
		char	   *sql = psprintf("SELECT last_value::text, is_called::text FROM %s",
								   quote_qualified_identifier(schema, ws_sql_value(&seqs, i, 0)));
		WsSqlRows	at;

		WsSql->run(sql, 0, NULL, &at);
		(*out)[i].name = ws_name_to_utf8(ws_sql_value(&seqs, i, 0));
		(*out)[i].last_value = strtoi64(ws_sql_value(&at, 0, 0), NULL, 10);
		(*out)[i].is_called = ws_sql_value(&at, 0, 1)[0] == 't';
		ws_sql_rows_free(&at);
		pfree(sql);
	}
	ws_sql_rows_free(&seqs);
	if (*n > 1)
		qsort(*out, *n, sizeof(WsSeqPosition), position_cmp);
	WsSql->end(level);
}

void
ws_seq_restore(const WsSeqPosition *positions, int n, const char *schema)
{
	int			level = WsSql->begin();

	for (int i = 0; i < n; i++)
	{
		char	   *here = ws_name_from_utf8(positions[i].name);
		char		value[32];
		const char *params[3] = {quote_qualified_identifier(schema, here), value, positions[i].is_called ? "t" : "f"};
		WsSqlRows	rows;

		snprintf(value, sizeof(value), INT64_FORMAT, positions[i].last_value);
		WsSql->run("SELECT pg_catalog.setval($1::regclass, $2::bigint, $3::boolean)", 3, params, &rows);
		ws_sql_rows_free(&rows);
		pfree(here);
	}
	WsSql->end(level);
}

/* ------------------------------------- collations, conversions and casts */

static void check_of_table_is_portable(const char *name, const char *schema, const char *statement);

static const char *const collation_refusal_sql =
"SELECT o.collname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_collation'::regclass AND d.objid = o.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension' "
"  WHEN EXISTS (SELECT 1 FROM pg_collation c WHERE c.collname = o.collname AND c.collnamespace = 'pg_catalog'::regnamespace "
"      AND c.collencoding IN (-1, pg_char_to_encoding(getdatabaseencoding()))) "
"    THEN 'it is named as one in the catalogs, which its bare name would mean' "
"  WHEN o.collencoding NOT IN (-1, pg_char_to_encoding(getdatabaseencoding())) "
"    THEN 'it is for another encoding than this database''s' "
"  END FROM pg_collation o JOIN pg_namespace n ON n.oid = o.collnamespace WHERE n.nspname = $1::name ORDER BY 1";

static const char *const conversion_refusal_sql =
"SELECT o.conname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_conversion'::regclass AND d.objid = o.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension' "
"  WHEN o.condefault THEN 'it is a default conversion, which no commit holds yet' "
"  WHEN p.pronamespace <> 'pg_catalog'::regnamespace THEN 'its function is not one in the catalogs' "
"  END FROM pg_conversion o JOIN pg_namespace n ON n.oid = o.connamespace JOIN pg_proc p ON p.oid = o.conproc "
"WHERE n.nspname = $1::name ORDER BY 1";

/* Its owner, if it can travel, and its comments. */
static void
read_owner_and_comments(WsObject *o, const char *oid, const char *name, const char *owner_sql, const char *catalog)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;
	char	   *comments = psprintf("SELECT description FROM pg_description WHERE classoid = '%s'::regclass AND objoid = $1::oid", catalog);

	if (owner_sql != NULL)
	{
		WsSql->read(owner_sql, 1, params, &rows);
		if (rows.nrows == 1 && !IsReservedName(ws_sql_value(&rows, 0, 0)))
			o->owner = role_in_commit(name, ws_sql_value(&rows, 0, 0));
		ws_sql_rows_free(&rows);
	}
	WsSql->read(comments, 1, params, &rows);
	o->comments = zeroed(rows.nrows, sizeof(WsObjectComment));
	o->ncomments = rows.nrows;
	for (int c = 0; c < rows.nrows; c++)
		o->comments[c].text = ws_text_to_utf8(ws_sql_value(&rows, c, 0));
	ws_sql_rows_free(&rows);
	pfree(comments);
}

static void
set_definition(WsObject *o, const char *text)
{
	Buf			stored = BUF_INIT;

	ws_tags_escape(ws_text_to_utf8(text), &stored);
	buf_append_byte(&stored, 0);
	o->definition = px_memdup(stored.data, stored.len);
	buf_free(&stored);
	if (o->deps == NULL)
		o->deps = zeroed(0, sizeof(char *));
}

/* Adds the collations of a schema to objs, of *n and room for it. */
static WsObject *
read_collations(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read("SELECT o.oid::text, o.collname::text, o.collprovider::text, o.collisdeterministic::text, "
				"  o.collcollate, o.collctype, o.colllocale, o.collicurules "
				"FROM pg_collation o JOIN pg_namespace n ON n.oid = o.collnamespace WHERE n.nspname = $1::name", 1, params, &rows);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		const char *name = ws_sql_value(&rows, i, 1);
		char		provider = ws_sql_value(&rows, i, 2)[0];
		StringInfoData s;

		/* (no version: that is the library's here, as a restore will find its own) */
		initStringInfo(&s);
		appendStringInfo(&s, "(PROVIDER = %s", provider == 'i' ? "icu" : provider == 'b' ? "builtin" : "libc");
		if (provider == 'c')
			appendStringInfo(&s, ", LC_COLLATE = %s, LC_CTYPE = %s", quote_literal_cstr(ws_sql_value(&rows, i, 4)),
							 quote_literal_cstr(ws_sql_value(&rows, i, 5)));
		else
			appendStringInfo(&s, ", LOCALE = %s", quote_literal_cstr(ws_sql_value(&rows, i, 6)));
		if (ws_sql_value(&rows, i, 3)[0] != 't')
			appendStringInfoString(&s, ", DETERMINISTIC = false");
		if (ws_sql_value(&rows, i, 7) != NULL)
			appendStringInfo(&s, ", RULES = %s", quote_literal_cstr(ws_sql_value(&rows, i, 7)));
		appendStringInfoChar(&s, ')');
		o->kind = WS_OBJ_COLLATION;
		o->name = ws_name_to_utf8(name);
		set_definition(o, s.data);
		read_owner_and_comments(o, oid, name,
								"SELECT r.rolname::text FROM pg_collation o JOIN pg_roles r ON r.oid = o.collowner WHERE o.oid = $1::oid "
								"AND NOT (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)",
								"pg_collation");
	}
	ws_sql_rows_free(&rows);
	return objs;
}

/* Adds the conversions of a schema to objs, of *n and room for it. */
static WsObject *
read_conversions(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read("SELECT o.oid::text, o.conname::text, pg_encoding_to_char(o.conforencoding)::text, "
				"  pg_encoding_to_char(o.contoencoding)::text, quote_ident(p.proname) "
				"FROM pg_conversion o JOIN pg_namespace n ON n.oid = o.connamespace JOIN pg_proc p ON p.oid = o.conproc "
				"WHERE n.nspname = $1::name", 1, params, &rows);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		const char *name = ws_sql_value(&rows, i, 1);

		o->kind = WS_OBJ_CONVERSION;
		o->name = ws_name_to_utf8(name);
		set_definition(o, psprintf("FOR %s TO %s FROM %s", quote_literal_cstr(ws_sql_value(&rows, i, 2)),
								   quote_literal_cstr(ws_sql_value(&rows, i, 3)), ws_sql_value(&rows, i, 4)));
		read_owner_and_comments(o, oid, name,
								"SELECT r.rolname::text FROM pg_conversion o JOIN pg_roles r ON r.oid = o.conowner WHERE o.oid = $1::oid "
								"AND NOT (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)",
								"pg_conversion");
	}
	ws_sql_rows_free(&rows);
	return objs;
}

/* Adds the casts of a schema to objs, of *n and room for it. */
static WsObject *
read_casts(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read("SELECT k.oid::text, k.castsource::text, k.casttarget::text, k.castfunc::text, k.castcontext::text, k.castmethod::text "
				CASTS_OF_SCHEMA, 1, params, &rows);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		char	   *source = print_type(schema, atooid(ws_sql_value(&rows, i, 1)), -1);
		char	   *target = print_type(schema, atooid(ws_sql_value(&rows, i, 2)), -1);
		char		method = ws_sql_value(&rows, i, 5)[0];
		char		context = ws_sql_value(&rows, i, 4)[0];
		StringInfoData s;
		char	   *name;

		initStringInfo(&s);
		appendStringInfo(&s, "CREATE CAST (%s AS %s) ", source, target);
		if (method == 'f')
		{
			Oid			fn = atooid(ws_sql_value(&rows, i, 3));

			/* (read under an old snapshot, it may be gone by now) */
			appendStringInfo(&s, "WITH FUNCTION %s(%s)", quote_identifier(printed_or_retry(schema, get_func_name(fn))),
							 function_args(schema, fn, NULL));
		}
		else
			appendStringInfoString(&s, method == 'i' ? "WITH INOUT" : "WITHOUT FUNCTION");
		if (context == 'a')
			appendStringInfoString(&s, " AS ASSIGNMENT");
		else if (context == 'i')
			appendStringInfoString(&s, " AS IMPLICIT");
		name = psprintf("%s", source);
		check_of_table_is_portable(name, schema, s.data);
		o->kind = WS_OBJ_CAST;
		o->name = ws_text_to_utf8(source);
		o->args = ws_text_to_utf8(target);
		set_definition(o, s.data);
		px_free(o->deps);
		o->deps = NULL;
		/* (no cast is a type or function, whatever its oid) */
		read_deps_of(o, schema, oid, "SELECT 'pg_cast'::regclass::oid, $1::oid", "0");
		read_owner_and_comments(o, oid, name, NULL, "pg_cast");
		pfree(name);
	}
	ws_sql_rows_free(&rows);
	return objs;
}

/* ------------------------------------- operators, their classes and families */

#define NEEDS_ANOTHER_SCHEMA(source) \
"  WHEN EXISTS (SELECT 1 FROM " source " JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid " \
"      CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i " \
"      WHERE d.deptype IN ('n', 'a') AND i.schema IS NOT NULL AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) " \
"    THEN 'it needs something in another schema' "

/* A table's row type, or an array of one, among some types: a table is made after these. */
#define ROW_TYPE_AMONG(types) \
"  WHEN EXISTS (SELECT 1 FROM " types " JOIN pg_type ty ON ty.oid = u.t LEFT JOIN pg_type e ON e.oid = ty.typelem AND e.typarray = ty.oid " \
"      JOIN pg_class q ON q.oid = coalesce(e.typrelid, ty.typrelid) WHERE q.relkind <> 'c') " \
"    THEN 'it is of a table''s or a view''s row type, and a table may need it' "

static const char *const operator_refusal_sql =
"SELECT o.oprname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_operator'::regclass AND d.objid = o.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension' "
"  WHEN EXISTS (SELECT 1 FROM pg_operator c WHERE c.oprname = o.oprname AND c.oprnamespace = 'pg_catalog'::regnamespace) "
"    THEN 'it is named as one in the catalogs' "
"  WHEN o.oprcode = 0 THEN 'it is a shell, named by another operator and never made' "
"  WHEN EXISTS (SELECT 1 FROM pg_operator x WHERE x.oid IN (o.oprcom, o.oprnegate) AND x.oprnamespace <> o.oprnamespace) "
"    THEN 'its commutator or negator is in another schema' "
/* (setting one sets both, so a link only one side says could not be made again) */
"  WHEN EXISTS (SELECT 1 FROM pg_operator x WHERE x.oid = o.oprcom AND x.oprcom <> o.oid) "
"    THEN 'its commutator does not say it back, as ALTER OPERATOR SET would make it' "
"  WHEN EXISTS (SELECT 1 FROM pg_operator x WHERE x.oid = o.oprnegate AND x.oprnegate <> o.oid) "
"    THEN 'its negator does not say it back, as ALTER OPERATOR SET would make it' "
NEEDS_ANOTHER_SCHEMA("(SELECT 'pg_operator'::regclass::oid, o.oid) s(classid, objid)")
ROW_TYPE_AMONG("unnest(ARRAY[o.oprleft, o.oprright]) u(t)")
"  END FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace WHERE n.nspname = $1::name ORDER BY 1";

static const char *const opfamily_refusal_sql =
"SELECT o.opfname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_opfamily'::regclass AND d.objid = o.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension' "
"  WHEN EXISTS (SELECT 1 FROM pg_opfamily c WHERE c.opfname = o.opfname AND c.opfmethod = o.opfmethod AND c.opfnamespace = 'pg_catalog'::regnamespace) "
"    THEN 'it is named as one in the catalogs' "
"  WHEN EXISTS (SELECT 1 FROM pg_opclass c WHERE c.opcfamily = o.oid AND c.opcnamespace <> o.opfnamespace) "
"    THEN 'a class of it is in another schema' "
NEEDS_ANOTHER_SCHEMA("(SELECT 'pg_amop'::regclass::oid, a.oid FROM pg_amop a WHERE a.amopfamily = o.oid UNION ALL SELECT 'pg_amproc'::regclass::oid, a.oid FROM pg_amproc a WHERE a.amprocfamily = o.oid) s(classid, objid)")
ROW_TYPE_AMONG("(SELECT x FROM pg_amop a, unnest(ARRAY[a.amoplefttype, a.amoprighttype]) x WHERE a.amopfamily = o.oid "
			   "UNION SELECT x FROM pg_amproc a, unnest(ARRAY[a.amproclefttype, a.amprocrighttype]) x WHERE a.amprocfamily = o.oid) u(t)")
"  END FROM pg_opfamily o JOIN pg_namespace n ON n.oid = o.opfnamespace WHERE n.nspname = $1::name ORDER BY 1";

static const char *const opclass_refusal_sql =
"SELECT o.opcname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_opclass'::regclass AND d.objid = o.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension' "
"  WHEN EXISTS (SELECT 1 FROM pg_opclass c WHERE c.opcname = o.opcname AND c.opcmethod = o.opcmethod AND c.opcnamespace = 'pg_catalog'::regnamespace) "
"    THEN 'it is named as one in the catalogs' "
"  WHEN f.opfnamespace <> o.opcnamespace THEN 'its family is in another schema' "
"  WHEN o.opcdefault AND EXISTS (SELECT 1 FROM pg_type t WHERE t.oid = o.opcintype AND t.typnamespace <> o.opcnamespace) "
"    THEN 'it is a default for a type in the catalogs, which where it arrives may already have' "
NEEDS_ANOTHER_SCHEMA("(SELECT 'pg_opclass'::regclass::oid, o.oid) s(classid, objid)")
ROW_TYPE_AMONG("unnest(ARRAY[o.opcintype, o.opckeytype]) u(t)")
"  END FROM pg_opclass o JOIN pg_namespace n ON n.oid = o.opcnamespace JOIN pg_opfamily f ON f.oid = o.opcfamily "
"WHERE n.nspname = $1::name ORDER BY 1";

/* What is of a schema, and held by a family that is not, would be lost. */
static const char *const member_refusal_sql =
"SELECT x.oprname::text, 'it is a member of a family in another schema' FROM pg_amop a JOIN pg_operator x ON x.oid = a.amopopr "
"JOIN pg_opfamily f ON f.oid = a.amopfamily JOIN pg_namespace n ON n.oid = x.oprnamespace "
"WHERE n.nspname = $1::name AND f.opfnamespace <> x.oprnamespace "
"UNION ALL SELECT p.proname::text, 'it is a member of a family in another schema' FROM pg_amproc a JOIN pg_proc p ON p.oid = a.amproc "
"JOIN pg_opfamily f ON f.oid = a.amprocfamily JOIN pg_namespace n ON n.oid = p.pronamespace "
"WHERE n.nspname = $1::name AND f.opfnamespace <> p.pronamespace ORDER BY 1";

/* A function's name, or one to try again: read under an old snapshot, it may be gone. */
static const char *
function_name_or_retry(const char *schema, Oid fn)
{
	return quote_identifier(printed_or_retry(schema, get_func_name(fn)));
}

#define MEMBER_OF_CLASS "d.refclassid = 'pg_opclass'::regclass AND d.deptype = 'i'"
#define MEMBER_OF_FAMILY "d.refclassid = 'pg_opfamily'::regclass AND d.deptype IN ('a', 'i')"

/* An item of a class or family, to sort by, as nothing else is the same on every server. */
typedef struct OpItem
{
	int			is_function;
	int			number;
	char	   *text;
	char	   *in_utf8;		/* to sort by: an encoding's order is its own */
} OpItem;

static int
op_item_cmp(const void *a, const void *b)
{
	const OpItem *x = a;
	const OpItem *y = b;

	if (x->is_function != y->is_function)
		return x->is_function - y->is_function;
	if (x->number != y->number)
		return x->number - y->number;
	return strcmp(x->in_utf8, y->in_utf8);
}

/*
 * The items of a class or family, as its statement says them: of what depends on it,
 * a class's its own and a family's those of no class, soft or, as an access method may
 * have a required one, hard.
 */
static char *
op_items(const char *schema, const char *oid, bool is_class)
{
	const char *params[1] = {oid};
	WsSqlRows	ops;
	WsSqlRows	procs;
	OpItem	   *items;
	int			n = 0;
	StringInfoData s;
	const char *of = is_class ? MEMBER_OF_CLASS : MEMBER_OF_FAMILY;
	char	   *ops_sql = psprintf("SELECT a.amopstrategy::text, x.oprname::text, a.amoplefttype::text, a.amoprighttype::text, "
								   "  a.amoppurpose::text, a.amopsortfamily::text "
								   "FROM pg_amop a JOIN pg_operator x ON x.oid = a.amopopr "
								   "JOIN pg_depend d ON d.classid = 'pg_amop'::regclass AND d.objid = a.oid "
								   "WHERE %s AND d.refobjid = $1::oid", of);
	char	   *procs_sql = psprintf("SELECT a.amprocnum::text, a.amproclefttype::text, a.amprocrighttype::text, a.amproc::oid::text "
									 "FROM pg_amproc a JOIN pg_depend d ON d.classid = 'pg_amproc'::regclass AND d.objid = a.oid "
									 "WHERE %s AND d.refobjid = $1::oid", of);

	WsSql->read(ops_sql, 1, params, &ops);
	WsSql->read(procs_sql, 1, params, &procs);
	items = palloc_array(OpItem, Max(ops.nrows + procs.nrows, 1));
	for (int i = 0; i < ops.nrows; i++)
	{
		StringInfoData one;

		initStringInfo(&one);
		appendStringInfo(&one, "OPERATOR %s %s(%s)", ws_sql_value(&ops, i, 0), ws_sql_value(&ops, i, 1),
						 operator_args(schema, atooid(ws_sql_value(&ops, i, 2)), atooid(ws_sql_value(&ops, i, 3))));
		if (ws_sql_value(&ops, i, 4)[0] == 'o')
		{
			Buf			key = BUF_INIT;
			WsSqlRows	fam;
			const char *fp[1] = {ws_sql_value(&ops, i, 5)};

			WsSql->read("SELECT opfname::text FROM pg_opfamily WHERE oid = $1::oid", 1, fp, &fam);
			if (fam.nrows != 1)
				printed_or_retry(schema, NULL);
			appendStringInfo(&one, " FOR ORDER BY %s", quote_identifier(ws_sql_value(&fam, 0, 0)));
			ws_sql_rows_free(&fam);
			buf_free(&key);
		}
		items[n++] = (OpItem) {0, atoi(ws_sql_value(&ops, i, 0)), one.data, ws_text_to_utf8(one.data)};
	}
	for (int i = 0; i < procs.nrows; i++)
	{
		Oid			fn = atooid(ws_sql_value(&procs, i, 3));

		char	   *text = psprintf("FUNCTION %s (%s) %s(%s)", ws_sql_value(&procs, i, 0),
									operator_args(schema, atooid(ws_sql_value(&procs, i, 1)), atooid(ws_sql_value(&procs, i, 2))),
									function_name_or_retry(schema, fn), function_args(schema, fn, NULL));

		items[n++] = (OpItem) {1, atoi(ws_sql_value(&procs, i, 0)), text, ws_text_to_utf8(text)};
	}
	if (n > 1)
		qsort(items, n, sizeof(OpItem), op_item_cmp);
	initStringInfo(&s);
	for (int i = 0; i < n; i++)
		appendStringInfo(&s, "%s%s", i > 0 ? ", " : "", items[i].text);
	ws_sql_rows_free(&ops);
	ws_sql_rows_free(&procs);
	pfree(ops_sql);
	pfree(procs_sql);
	return n > 0 ? s.data : NULL;
}

/* One more need, of what read_deps does not see. */
static void
add_dep(WsObject *o, Buf *key)
{
	char	  **deps = zeroed(o->ndeps + 1, sizeof(char *));
	bool		have = false;

	buf_append_byte(key, 0);
	for (int i = 0; i < o->ndeps; i++)
	{
		deps[i] = o->deps[i];
		have |= strcmp(deps[i], (char *) key->data) == 0;
	}
	if (!have)
		deps[o->ndeps++] = px_memdup(key->data, key->len);
	px_free(o->deps);
	o->deps = deps;
	if (o->ndeps > 1)
		qsort(o->deps, o->ndeps, sizeof(char *), cstr_cmp);
	buf_free(key);
}

/*
 * The types of an item are in its row, and not all are depended on, as a function
 * taking oid may be for any type: what the items of one class or family (of $1) say.
 */
static void
add_member_type_needs(WsObject *o, const char *schema, const char *oid, const char *of)
{
	const char *params[2] = {oid, schema};
	WsSqlRows	rows;
	char	   *sql = psprintf("WITH n(typ) AS ("
							   "  SELECT x FROM pg_amop a JOIN pg_depend d ON d.classid = 'pg_amop'::regclass AND d.objid = a.oid, "
							   "    unnest(ARRAY[a.amoplefttype, a.amoprighttype]) x WHERE %s AND d.refobjid = $1::oid "
							   "  UNION SELECT x FROM pg_amproc a JOIN pg_depend d ON d.classid = 'pg_amproc'::regclass AND d.objid = a.oid, "
							   "    unnest(ARRAY[a.amproclefttype, a.amprocrighttype]) x WHERE %s AND d.refobjid = $1::oid) "
							   "SELECT DISTINCT CASE b.typtype WHEN 'd' THEN 'D' ELSE 'T' END, b.typname::text FROM n "
							   "JOIN pg_type t ON t.oid = n.typ "
							   "LEFT JOIN pg_type e ON e.oid = t.typelem AND e.typarray = t.oid "
							   "LEFT JOIN pg_range r ON r.rngmultitypid = coalesce(e.oid, t.oid) "
							   "JOIN pg_type b ON b.oid = coalesce(r.rngtypid, e.oid, t.oid) "
							   "WHERE b.typnamespace = (SELECT oid FROM pg_namespace WHERE nspname = $2::name)", of, of);

	WsSql->read(sql, 2, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
	{
		Buf			key = BUF_INIT;

		ws_obj_key(ws_sql_value(&rows, i, 0)[0] == 'D' ? WS_OBJ_DOMAIN : WS_OBJ_TYPE,
				   ws_name_to_utf8(ws_sql_value(&rows, i, 1)), NULL, &key);
		add_dep(o, &key);
	}
	ws_sql_rows_free(&rows);
	pfree(sql);
}

#define OWNER_IF_IT_CAN_TRAVEL(catalog, column) \
	"SELECT r.rolname::text FROM " catalog " o JOIN pg_roles r ON r.oid = o." column " WHERE o.oid = $1::oid " \
	"AND NOT (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)"

static WsObject *
read_operators(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read("SELECT o.oid::text, o.oprname::text, o.oprleft::text, o.oprright::text, o.oprcode::oid::text, o.oprrest::oid::text, "
				"  o.oprjoin::oid::text, o.oprcanhash::text, o.oprcanmerge::text, o.oprcom::text, o.oprnegate::text "
				"FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace WHERE n.nspname = $1::name", 1, params, &rows);
	/* (a link may be needed for each) */
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + 2 * rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		const char *name = ws_sql_value(&rows, i, 1);
		Oid			left = atooid(ws_sql_value(&rows, i, 2));
		Oid			right = atooid(ws_sql_value(&rows, i, 3));
		Oid			rest = atooid(ws_sql_value(&rows, i, 5));
		Oid			join = atooid(ws_sql_value(&rows, i, 6));
		Oid			com = atooid(ws_sql_value(&rows, i, 9));
		Oid			neg = atooid(ws_sql_value(&rows, i, 10));
		char	   *args = operator_args(schema, left, right);
		StringInfoData s;

		initStringInfo(&s);
		appendStringInfoChar(&s, '(');
		if (OidIsValid(left))
			appendStringInfo(&s, "LEFTARG = %s, ", print_type(schema, left, -1));
		if (OidIsValid(right))
			appendStringInfo(&s, "RIGHTARG = %s, ", print_type(schema, right, -1));
		appendStringInfo(&s, "FUNCTION = %s", function_name_or_retry(schema, atooid(ws_sql_value(&rows, i, 4))));
		if (OidIsValid(rest))
			appendStringInfo(&s, ", RESTRICT = %s", function_name_or_retry(schema, rest));
		if (OidIsValid(join))
			appendStringInfo(&s, ", JOIN = %s", function_name_or_retry(schema, join));
		if (ws_sql_value(&rows, i, 7)[0] == 't')
			appendStringInfoString(&s, ", HASHES");
		if (ws_sql_value(&rows, i, 8)[0] == 't')
			appendStringInfoString(&s, ", MERGES");
		appendStringInfoChar(&s, ')');
		check_of_table_is_portable(name, schema, psprintf("CREATE OPERATOR %s %s", name, s.data));
		o->kind = WS_OBJ_OPERATOR;
		o->name = ws_name_to_utf8(name);
		o->args = ws_text_to_utf8(args);
		set_definition(o, s.data);
		px_free(o->deps);
		o->deps = NULL;
		/* (no operator, class or member is a type or function, whatever its oid) */
		read_deps_of(o, schema, oid, "SELECT 'pg_operator'::regclass::oid, $1::oid", "0");
		read_owner_and_comments(o, oid, name, OWNER_IF_IT_CAN_TRAVEL("pg_operator", "oprowner"), "pg_operator");
		if (OidIsValid(com) || OidIsValid(neg))
		{
			WsObject   *link = &objs[(*n)++];
			Buf			self = BUF_INIT;
			StringInfoData l;
			WsSqlRows	names;
			char	   *comtext = psprintf("%u", com);
			char	   *negtext = psprintf("%u", neg);
			const char *lp[2] = {comtext, negtext};

			WsSql->read("SELECT (SELECT oprname::text FROM pg_operator WHERE oid = $1::oid), "
						"(SELECT oprname::text FROM pg_operator WHERE oid = $2::oid)", 2, lp, &names);
			initStringInfo(&l);
			appendStringInfo(&l, "ALTER OPERATOR %s (%s) SET (", name, args);
			if (OidIsValid(com))
				appendStringInfo(&l, "COMMUTATOR = %s", ws_sql_value(&names, 0, 0));
			if (OidIsValid(neg))
				appendStringInfo(&l, "%sNEGATOR = %s", OidIsValid(com) ? ", " : "", ws_sql_value(&names, 0, 1));
			appendStringInfoChar(&l, ')');
			link->kind = WS_OBJ_OPLINK;
			link->name = ws_name_to_utf8(name);
			link->args = ws_text_to_utf8(args);
			set_definition(link, l.data);
			ws_obj_key(WS_OBJ_OPERATOR, link->name, link->args, &self);
			add_dep(link, &self);
			if (OidIsValid(com))
			{
				Buf			key = BUF_INIT;

				operator_key(schema, com, &key);
				add_dep(link, &key);
			}
			if (OidIsValid(neg))
			{
				Buf			key = BUF_INIT;

				operator_key(schema, neg, &key);
				add_dep(link, &key);
			}
			ws_sql_rows_free(&names);
			pfree(comtext);
			pfree(negtext);
		}
	}
	ws_sql_rows_free(&rows);
	return objs;
}

static WsObject *
read_opfamilies(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read("SELECT o.oid::text, o.opfname::text, quote_ident(a.amname) FROM pg_opfamily o JOIN pg_am a ON a.oid = o.opfmethod "
				"JOIN pg_namespace n ON n.oid = o.opfnamespace WHERE n.nspname = $1::name", 1, params, &rows);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + 2 * rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		const char *name = ws_sql_value(&rows, i, 1);
		const char *am = ws_sql_value(&rows, i, 2);
		char	   *members = op_items(schema, oid, false);

		o->kind = WS_OBJ_OPFAMILY;
		o->name = ws_name_to_utf8(name);
		o->args = ws_name_to_utf8(am);
		set_definition(o, psprintf("USING %s", am));
		read_owner_and_comments(o, oid, name, OWNER_IF_IT_CAN_TRAVEL("pg_opfamily", "opfowner"), "pg_opfamily");
		if (members != NULL)
		{
			WsObject   *m = &objs[(*n)++];
			Buf			key = BUF_INIT;
			char	   *sql = psprintf("ALTER OPERATOR FAMILY %s USING %s ADD %s", quote_identifier(name), am, members);

			check_of_table_is_portable(name, schema, sql);
			m->kind = WS_OBJ_OPMEMBERS;
			m->name = ws_name_to_utf8(name);
			m->args = ws_name_to_utf8(am);
			set_definition(m, sql);
			px_free(m->deps);
			m->deps = NULL;
			read_deps_of(m, schema, oid,
						 "SELECT d.classid, d.objid FROM pg_depend d WHERE d.refclassid = 'pg_opfamily'::regclass AND d.refobjid = $1::oid "
						 "AND d.deptype IN ('a', 'i') AND d.classid IN ('pg_amop'::regclass, 'pg_amproc'::regclass)", "0");
			ws_obj_key(WS_OBJ_OPFAMILY, o->name, o->args, &key);
			add_dep(m, &key);
			add_member_type_needs(m, schema, oid, MEMBER_OF_FAMILY);
		}
	}
	ws_sql_rows_free(&rows);
	return objs;
}

static WsObject *
read_opclasses(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->read("SELECT o.oid::text, o.opcname::text, quote_ident(a.amname), o.opcdefault::text, o.opcintype::text, o.opckeytype::text, f.opfname::text "
				"FROM pg_opclass o JOIN pg_am a ON a.oid = o.opcmethod JOIN pg_opfamily f ON f.oid = o.opcfamily "
				"JOIN pg_namespace n ON n.oid = o.opcnamespace WHERE n.nspname = $1::name", 1, params, &rows);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		const char *name = ws_sql_value(&rows, i, 1);
		const char *am = ws_sql_value(&rows, i, 2);
		Oid			storage = atooid(ws_sql_value(&rows, i, 5));
		char	   *items = op_items(schema, oid, true);
		Buf			family = BUF_INIT;
		StringInfoData s;

		initStringInfo(&s);
		appendStringInfo(&s, "%sFOR TYPE %s USING %s FAMILY %s AS ", ws_sql_value(&rows, i, 3)[0] == 't' ? "DEFAULT " : "",
						 print_type(schema, atooid(ws_sql_value(&rows, i, 4)), -1), am, quote_identifier(ws_sql_value(&rows, i, 6)));
		if (items != NULL)
			appendStringInfoString(&s, items);
		if (OidIsValid(storage))
			appendStringInfo(&s, "%sSTORAGE %s", items != NULL ? ", " : "", print_type(schema, storage, -1));
		if (items == NULL && !OidIsValid(storage))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: it has no members of its own, so no statement could make it",
							name, schema)));
		check_of_table_is_portable(name, schema, psprintf("CREATE OPERATOR CLASS %s %s", quote_identifier(name), s.data));
		o->kind = WS_OBJ_OPCLASS;
		o->name = ws_name_to_utf8(name);
		o->args = ws_name_to_utf8(am);
		set_definition(o, s.data);
		px_free(o->deps);
		o->deps = NULL;
		read_deps_of(o, schema, oid,
					 "SELECT 'pg_opclass'::regclass::oid, $1::oid UNION ALL SELECT d.classid, d.objid FROM pg_depend d "
					 "WHERE d.refclassid = 'pg_opclass'::regclass AND d.refobjid = $1::oid AND d.deptype = 'i' "
					 "AND d.classid IN ('pg_amop'::regclass, 'pg_amproc'::regclass)", "0");
		ws_obj_key(WS_OBJ_OPFAMILY, ws_name_to_utf8(ws_sql_value(&rows, i, 6)), o->args, &family);
		add_dep(o, &family);
		add_member_type_needs(o, schema, oid, MEMBER_OF_CLASS);

		/*
		 * A member added once its class is there is tied to it, so what its family
		 * holds outside any class goes first, as those are what was not.
		 */
		for (int m = 0; m < *n; m++)
			if (objs[m].kind == WS_OBJ_OPMEMBERS && strcmp(objs[m].args, o->args) == 0 &&
				strcmp(objs[m].name, ws_name_to_utf8(ws_sql_value(&rows, i, 6))) == 0)
			{
				Buf			members = BUF_INIT;

				ws_obj_key(WS_OBJ_OPMEMBERS, objs[m].name, objs[m].args, &members);
				add_dep(o, &members);
			}
		read_owner_and_comments(o, oid, name, OWNER_IF_IT_CAN_TRAVEL("pg_opclass", "opcowner"), "pg_opclass");
	}
	ws_sql_rows_free(&rows);
	return objs;
}

/* ------------------------------------- triggers, rules and policies */

/* What such a one needs that no commit can hold, of its row in a catalog and its source. */
#define OF_TABLE_NEEDS(catalog, oid) \
"  WHEN c.relkind NOT IN ('r', 'p', 'v', 'm') THEN 'it is on what no commit holds' " \
"  WHEN EXISTS (SELECT 1 FROM pg_class k WHERE k.relname = c.relname AND k.relnamespace = 'pg_catalog'::regnamespace) " \
"    THEN 'its relation is named as one in the catalogs, which its bare name would be taken for' " \
"  WHEN EXISTS (SELECT 1 FROM pg_depend d CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i " \
"      WHERE d.classid = '" catalog "'::regclass AND d.objid = " oid " AND d.deptype = 'n' AND i.schema IS NOT NULL " \
"        AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) THEN 'it needs something in another schema' " \
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = '" catalog "'::regclass AND d.objid = " oid " AND d.deptype = 'n' " \
"      AND NOT (d.refclassid IN ('pg_type'::regclass, 'pg_proc'::regclass, 'pg_collation'::regclass, 'pg_operator'::regclass, 'pg_opclass'::regclass, 'pg_opfamily'::regclass) " \
"        OR (d.refclassid = 'pg_class'::regclass AND EXISTS (SELECT 1 FROM pg_class q WHERE q.oid = d.refobjid " \
"          AND q.relkind IN ('r', 'p', 'v', 'm', 'c', 'S'))) " \
"        OR (d.refclassid = 'pg_constraint'::regclass AND EXISTS (SELECT 1 FROM pg_constraint k WHERE k.oid = d.refobjid AND k.conrelid <> 0)) " \
"        OR coalesce((SELECT i.schema = 'pg_catalog' FROM pg_identify_object(d.refclassid, d.refobjid, 0) i), false))) " \
"    THEN 'it needs an object no commit holds yet' " \
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = '" catalog "'::regclass AND d.objid = " oid " AND d.deptype IN ('e', 'x')) " \
"    THEN 'it belongs to an extension, or depends on one' " \
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = '" catalog "'::regclass AND l.objoid = " oid ") THEN 'it has a security label' "

static const char *const trigger_refusal_sql =
"SELECT g.tgname::text, CASE "
"  WHEN g.tgconstrrelid <> 0 AND NOT EXISTS (SELECT 1 FROM pg_class q WHERE q.oid = g.tgconstrrelid AND q.relkind IN ('r', 'p')) "
"    THEN 'it is a constraint trigger from what is no table' "
OF_TABLE_NEEDS("pg_trigger", "g.oid")
"  END FROM pg_trigger g JOIN pg_class c ON c.oid = g.tgrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
"WHERE n.nspname = $1::name AND NOT g.tgisinternal AND g.tgparentid = 0 ORDER BY 1";

/* A partition's clone comes with attaching, as its table's is, so must be no other. */
static const char *const trigger_clone_refusal_sql =
"SELECT g.tgname::text, CASE "
"  WHEN g.tgenabled <> u.tgenabled THEN 'it is a partition''s, and fires otherwise than its table''s' "
"  WHEN EXISTS (SELECT 1 FROM pg_description m WHERE m.classoid = 'pg_trigger'::regclass AND m.objoid = g.oid) "
"    THEN 'it is a partition''s, with a comment of its own' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_trigger'::regclass AND d.objid = g.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it is a partition''s, and depends on an extension' "
"  END FROM pg_trigger g JOIN pg_trigger u ON u.oid = g.tgparentid JOIN pg_class c ON c.oid = g.tgrelid "
"JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name ORDER BY 1";

static const char *const rule_refusal_sql =
"SELECT w.rulename::text, CASE "
OF_TABLE_NEEDS("pg_rewrite", "w.oid")
"  END FROM pg_rewrite w JOIN pg_class c ON c.oid = w.ev_class JOIN pg_namespace n ON n.oid = c.relnamespace "
"WHERE n.nspname = $1::name AND w.rulename <> '_RETURN' ORDER BY 1";

static const char *const index_refusal_sql =
"SELECT ic.relname::text, CASE "
OF_TABLE_NEEDS("pg_class", "ic.oid")
"  END FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid JOIN pg_class c ON c.oid = i.indrelid "
"JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind = 'm' ORDER BY 1";

static const char *const default_refusal_sql =
"SELECT a.attname::text, CASE "
OF_TABLE_NEEDS("pg_attrdef", "ad.oid")
"  END FROM pg_attrdef ad JOIN pg_class c ON c.oid = ad.adrelid JOIN pg_attribute a ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
"JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind = 'v' ORDER BY 1";

static const char *const policy_refusal_sql =
"SELECT y.polname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM unnest(y.polroles) u(o) JOIN pg_roles r ON r.oid = u.o WHERE r.rolcanlogin OR r.rolsuper "
"      OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls OR r.rolname ~ '^pg_') "
"    THEN 'it is for a role that cannot travel' "
OF_TABLE_NEEDS("pg_policy", "y.oid")
"  END FROM pg_policy y JOIN pg_class c ON c.oid = y.polrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
"WHERE n.nspname = $1::name ORDER BY 1";

/* Adds a need, in order, if it is not there. */
static void
add_need(WsObject *o, WsObjectKind kind, const char *name)
{
	Buf			key = BUF_INIT;
	char	  **deps;

	ws_obj_key(kind, name, NULL, &key);
	buf_append_byte(&key, 0);
	for (int i = 0; i < o->ndeps; i++)
		if (strcmp(o->deps[i], (char *) key.data) == 0)
		{
			buf_free(&key);
			return;
		}
	deps = zeroed(o->ndeps + 1, sizeof(char *));
	if (o->ndeps > 0)
		memcpy(deps, o->deps, o->ndeps * sizeof(char *));
	deps[o->ndeps++] = px_memdup(key.data, key.len);
	px_free(o->deps);
	o->deps = deps;
	qsort(o->deps, o->ndeps, sizeof(char *), cstr_cmp);
	buf_free(&key);
}

/* Refuses a trigger's, rule's or policy's statement that says a schema's name. */
static void
check_of_table_is_portable(const char *name, const char *schema, const char *sql)
{
	List	   *parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	Node	   *node = linitial_node(RawStmt, parsed)->stmt;
	NamesSchema n = {NULL, NULL};
	bool		named = false;

	if (IsA(node, CreateTrigStmt))
	{
		CreateTrigStmt *g = (CreateTrigStmt *) node;

		named = qualified(g->funcname, NULL) || names_schema_walker(g->whenClause, &n) ||
			(g->constrrel != NULL && names_schema_walker((Node *) g->constrrel, &n));
	}
	else if (IsA(node, RuleStmt))
	{
		ListCell   *lc;

		named = names_schema_walker(((RuleStmt *) node)->whereClause, &n);
		foreach(lc, ((RuleStmt *) node)->actions)
			if (!IsA(lfirst(lc), NotifyStmt))
				named = named || names_schema_walker(lfirst(lc), &n);
	}
	else if (IsA(node, CreatePolicyStmt))
		named = names_schema_walker(((CreatePolicyStmt *) node)->qual, &n) ||
			names_schema_walker(((CreatePolicyStmt *) node)->with_check, &n);
	else if (IsA(node, DefineStmt))
	{
		ListCell   *lc;

		/* (HASHES and MERGES have no argument) */
		foreach(lc, ((DefineStmt *) node)->definition)
		{
			Node	   *arg = lfirst_node(DefElem, lc)->arg;

			named |= arg != NULL && IsA(arg, TypeName) && names_schema_walker(arg, &n);
		}
	}
	else if (IsA(node, CreateOpClassStmt) || IsA(node, AlterOpFamilyStmt))
	{
		List	   *items = IsA(node, CreateOpClassStmt) ? ((CreateOpClassStmt *) node)->items : ((AlterOpFamilyStmt *) node)->items;
		ListCell   *lc;

		if (IsA(node, CreateOpClassStmt))
			named = names_schema_walker((Node *) ((CreateOpClassStmt *) node)->datatype, &n);
		foreach(lc, items)
		{
			CreateOpClassItem *item = lfirst_node(CreateOpClassItem, lc);
			ListCell   *lt;

			named |= names_schema_walker((Node *) item->storedtype, &n);
			if (item->name != NULL)
				foreach(lt, item->name->objargs)
					named |= names_schema_walker(lfirst(lt), &n);
			foreach(lt, item->class_args)
				named |= names_schema_walker(lfirst(lt), &n);
		}
	}
	else if (IsA(node, CreateCastStmt))
	{
		CreateCastStmt *k = (CreateCastStmt *) node;

		named = names_schema_walker((Node *) k->sourcetype, &n) || names_schema_walker((Node *) k->targettype, &n);
	}
	else if (IsA(node, IndexStmt))
	{
		IndexStmt  *ix = (IndexStmt *) node;
		ListCell   *lc;

		named = names_schema_walker(ix->whereClause, &n);
		foreach(lc, ix->indexParams)
		{
			IndexElem  *e = lfirst_node(IndexElem, lc);

			named = named || names_schema_walker(e->expr, &n) || qualified(e->collation, NULL) ||
				qualified(e->opclass, NULL);
		}
		foreach(lc, ix->indexIncludingParams)
			named = named || names_schema_walker(lfirst_node(IndexElem, lc)->expr, &n);
	}
	else if (IsA(node, AlterTableStmt))
	{
		AlterTableCmd *cmd = linitial_node(AlterTableCmd, ((AlterTableStmt *) node)->cmds);

		named = names_schema_walker(cmd->def, &n);
	}
	else
		elog(ERROR, "\"%s\" printed as something else", name);
	if (named)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: %s", name, schema,
						n.why ? n.why : "it must say a schema's name, as what it means has a name something else hides")));
}

/* A policy's statement, there being no printing function for one. */
static char *
policy_statement(const char *schema, const char *oid, const char *name, const char *table, Oid relid)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;
	StringInfoData s;

	{
		WsSqlRows	roles;

		WsSql->read("SELECT r.rolname::text FROM pg_policy y, unnest(y.polroles) u(o) JOIN pg_roles r ON r.oid = u.o "
					"WHERE y.oid = $1::oid", 1, params, &roles);
		for (int i = 0; i < roles.nrows; i++)
			pfree(role_in_commit(name, ws_sql_value(&roles, i, 0)));
		ws_sql_rows_free(&roles);
	}
	WsSql->read("SELECT y.polpermissive::text, y.polcmd::text, y.polqual::text, y.polwithcheck::text, "
				"  (SELECT string_agg(quote_ident(r.rolname), ', ' ORDER BY r.rolname) FROM unnest(y.polroles) u(o) "
				"    JOIN pg_roles r ON r.oid = u.o) FROM pg_policy y WHERE y.oid = $1::oid", 1, params, &rows);
	if (rows.nrows != 1)
		printed_or_retry(schema, NULL);
	initStringInfo(&s);
	appendStringInfo(&s, "CREATE POLICY %s ON %s AS %s FOR %s TO %s", quote_identifier(name), quote_identifier(table),
					 ws_sql_value(&rows, 0, 0)[0] == 't' ? "PERMISSIVE" : "RESTRICTIVE",
					 ws_sql_value(&rows, 0, 1)[0] == 'r' ? "SELECT" : ws_sql_value(&rows, 0, 1)[0] == 'a' ? "INSERT" :
					 ws_sql_value(&rows, 0, 1)[0] == 'w' ? "UPDATE" : ws_sql_value(&rows, 0, 1)[0] == 'd' ? "DELETE" : "ALL",
					 ws_sql_value(&rows, 0, 4) != NULL ? ws_sql_value(&rows, 0, 4) : "PUBLIC");
	if (ws_sql_value(&rows, 0, 2) != NULL)
		appendStringInfo(&s, " USING (%s)",
						 printed_or_retry(schema, ws_schema_print(schema, pg_get_expr, 2, CStringGetTextDatum(ws_sql_value(&rows, 0, 2)),
																  ObjectIdGetDatum(relid), 0)));
	if (ws_sql_value(&rows, 0, 3) != NULL)
		appendStringInfo(&s, " WITH CHECK (%s)",
						 printed_or_retry(schema, ws_schema_print(schema, pg_get_expr, 2, CStringGetTextDatum(ws_sql_value(&rows, 0, 3)),
																  ObjectIdGetDatum(relid), 0)));
	ws_sql_rows_free(&rows);
	return s.data;
}

/* The statement of one of a kind, naming its table bare. */
static char *
of_table_statement(WsObjectKind kind, const char *schema, const char *oid, const char *name, const char *table, Oid relid)
{
	char	   *printed;
	size_t		len;

	if (kind == WS_OBJ_POLICY)
		return policy_statement(schema, oid, name, table, relid);
	if (kind == WS_OBJ_DEFAULT)
	{
		const char *params[1] = {oid};
		WsSqlRows	rows;
		char	   *expr;

		WsSql->read("SELECT adbin::text FROM pg_attrdef WHERE oid = $1::oid", 1, params, &rows);
		if (rows.nrows != 1)
			printed_or_retry(schema, NULL);
		expr = printed_or_retry(schema, ws_schema_print(schema, pg_get_expr, 2, CStringGetTextDatum(ws_sql_value(&rows, 0, 0)),
														ObjectIdGetDatum(relid), 0));
		ws_sql_rows_free(&rows);
		return psprintf("ALTER VIEW %s ALTER COLUMN %s SET DEFAULT %s", quote_identifier(table), quote_identifier(name), expr);
	}
	printed = printed_or_retry(schema, ws_schema_print(schema, kind == WS_OBJ_TRIGGER ? pg_get_triggerdef :
													   kind == WS_OBJ_INDEX ? pg_get_indexdef : pg_get_ruledef, 1,
													   ObjectIdGetDatum(atooid(oid)), 0, 0));
	len = strlen(printed);
	while (len > 0 && (printed[len - 1] == ';' || printed[len - 1] == '\n' || printed[len - 1] == ' '))
		printed[--len] = '\0';
	{
		/*
		 * Each always prints its table's schema, which is the one thing it need not
		 * say.  Where it is, is the grammar's to say: a name or a column may hold the
		 * same text.
		 */
		char	   *qualified_name = quote_qualified_identifier(schema, table);
		List	   *parsed = raw_parser(printed, RAW_PARSE_DEFAULT);
		Node	   *node = linitial_node(RawStmt, parsed)->stmt;
		RangeVar   *on = IsA(node, CreateTrigStmt) ? ((CreateTrigStmt *) node)->relation :
			IsA(node, RuleStmt) ? ((RuleStmt *) node)->relation :
			IsA(node, IndexStmt) ? ((IndexStmt *) node)->relation : NULL;

		/* (a rename since what is read here: as what prints it reads the catalogs now) */
		if (on == NULL || on->location < 0 ||
			strncmp(printed + on->location, qualified_name, strlen(qualified_name)) != 0)
			printed_or_retry(schema, NULL);
		printed = psprintf("%.*s%s%s", on->location, printed, quote_identifier(table),
						   printed + on->location + strlen(qualified_name));
	}
	return printed;
}

/* Adds the triggers, rules or policies of a schema to objs, of *n and room for it. */
static WsObject *
read_of_table(const char *schema, WsObjectKind kind, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	const char *catalog = kind == WS_OBJ_TRIGGER ? "pg_trigger" : kind == WS_OBJ_RULE ? "pg_rewrite" :
		kind == WS_OBJ_INDEX ? "pg_class" : kind == WS_OBJ_DEFAULT ? "pg_attrdef" : "pg_policy";
	WsSqlRows	rows;

	WsSql->read(kind == WS_OBJ_TRIGGER ?
				"SELECT g.oid::text, g.tgname::text, c.relname::text, c.oid::text, c.relkind::text, g.tgenabled::text FROM pg_trigger g "
				"JOIN pg_class c ON c.oid = g.tgrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE n.nspname = $1::name AND NOT g.tgisinternal AND g.tgparentid = 0" :
				kind == WS_OBJ_RULE ?
				"SELECT w.oid::text, w.rulename::text, c.relname::text, c.oid::text, c.relkind::text, w.ev_enabled::text FROM pg_rewrite w "
				"JOIN pg_class c ON c.oid = w.ev_class JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE n.nspname = $1::name AND w.rulename <> '_RETURN'" :
				kind == WS_OBJ_INDEX ?
				"SELECT i.indexrelid::text, ic.relname::text, c.relname::text, c.oid::text, c.relkind::text FROM pg_index i "
				"JOIN pg_class ic ON ic.oid = i.indexrelid JOIN pg_class c ON c.oid = i.indrelid "
				"JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind = 'm'" :
				kind == WS_OBJ_DEFAULT ?
				"SELECT ad.oid::text, a.attname::text, c.relname::text, c.oid::text, c.relkind::text FROM pg_attrdef ad "
				"JOIN pg_class c ON c.oid = ad.adrelid JOIN pg_attribute a ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
				"JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind = 'v'" :
				"SELECT y.oid::text, y.polname::text, c.relname::text, c.oid::text, c.relkind::text FROM pg_policy y "
				"JOIN pg_class c ON c.oid = y.polrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE n.nspname = $1::name", 1, params, &rows);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&rows, i, 0);
		const char *name = ws_sql_value(&rows, i, 1);
		const char *table = ws_sql_value(&rows, i, 2);
		char		relkind = ws_sql_value(&rows, i, 4)[0];
		char	   *text = of_table_statement(kind, schema, oid, name, table, atooid(ws_sql_value(&rows, i, 3)));
		Buf			stored = BUF_INIT;
		char	   *source = psprintf("SELECT '%s'::regclass::oid, $1::oid", catalog);
		char	   *types = psprintf("SELECT refobjid::text FROM pg_depend WHERE classid = '%s'::regclass AND objid = $1::oid "
									 "AND deptype = 'n' AND refclassid = 'pg_type'::regclass", catalog);
		char	   *comments = psprintf("SELECT description FROM pg_description WHERE classoid = '%s'::regclass AND objoid = $1::oid", catalog);
		const char *self[1] = {oid};
		WsSqlRows	said;

		CHECK_FOR_INTERRUPTS();
		check_of_table_is_portable(name, schema, text);
		check_constants_hold_no_names(name, schema, oid, types);
		o->kind = kind;
		o->name = ws_name_to_utf8(name);
		o->args = ws_name_to_utf8(table);
		ws_tags_escape(ws_text_to_utf8(text), &stored);
		buf_append_byte(&stored, 0);
		o->definition = px_memdup(stored.data, stored.len);
		buf_free(&stored);
		if ((kind == WS_OBJ_TRIGGER || kind == WS_OBJ_RULE) && ws_sql_value(&rows, i, 5)[0] != 'O')
			o->fires = ws_sql_value(&rows, i, 5)[0];
		read_deps(o, schema, oid, source);
		if (relkind == RELKIND_VIEW || relkind == RELKIND_MATVIEW)
			add_need(o, relkind == RELKIND_VIEW ? WS_OBJ_VIEW : WS_OBJ_MATVIEW, o->args);
		WsSql->read(comments, 1, self, &said);
		o->comments = zeroed(said.nrows, sizeof(WsObjectComment));
		o->ncomments = said.nrows;
		for (int c = 0; c < said.nrows; c++)
			o->comments[c].text = ws_text_to_utf8(ws_sql_value(&said, c, 0));
		ws_sql_rows_free(&said);
		pfree(source);
		pfree(types);
		pfree(comments);
	}
	ws_sql_rows_free(&rows);
	return objs;
}

/* ------------------------------------------------------------ functions */

/* The functions of a schema that are objects of their own: not a range's constructors. */
#define FUNCTIONS_OF_SCHEMA \
	"FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace JOIN pg_language l ON l.oid = p.prolang " \
	"JOIN pg_roles r ON r.oid = p.proowner " \
	"WHERE n.nspname = $1::name AND NOT EXISTS (SELECT 1 FROM pg_depend x WHERE x.classid = 'pg_proc'::regclass " \
	"  AND x.objid = p.oid AND x.deptype = 'i' AND x.refclassid = 'pg_type'::regclass) "

static const char *const function_refusal_sql =
"SELECT p.proname::text, CASE "
"  WHEN p.prokind = 'a' THEN 'it is an aggregate, which no commit holds yet' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype IN ('e', 'x')) "
"    THEN 'it belongs to an extension, or depends on one' "
"  WHEN l.lanname IN ('c', 'internal') THEN 'it is in a language whose code no commit can bring' "
"  WHEN EXISTS (SELECT 1 FROM pg_proc c WHERE c.proname = p.proname AND c.pronamespace = 'pg_catalog'::regnamespace) "
"    THEN 'it is named as one in the catalogs, which a name could be taken for' "
"  WHEN p.prosecdef AND (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls "
"      OR r.rolname ~ '^pg_') THEN 'it is SECURITY DEFINER, and owned by a role that cannot travel' "
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel s WHERE s.classoid = 'pg_proc'::regclass AND s.objoid = p.oid) THEN 'it has a security label' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'n' "
"      AND d.refclassid = 'pg_proc'::regclass AND d.refobjid = p.oid) THEN 'it needs itself, as no commit could make it' "
"  WHEN p.procost = 'Infinity'::float4 OR p.prorows = 'Infinity'::float4 THEN 'its COST or ROWS is more than a definition can say' "
"  WHEN p.procost <> to_char(p.procost::float8::numeric, '9.99999EEEE')::float4 "
"      OR p.prorows <> to_char(p.prorows::float8::numeric, '9.99999EEEE')::float4 "
"    THEN 'its COST or ROWS has more digits than a definition prints' "
"  WHEN EXISTS (SELECT 1 FROM aclexplode(p.proacl) e WHERE e.grantor <> p.proowner) THEN 'it has a grant by a role that does not own it' "
"  WHEN p.proacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(p.proacl) e WHERE e.grantee = p.proowner) "
"    THEN 'its owner''s rights have been changed' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i "
"      WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'n' AND i.schema IS NOT NULL "
"        AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) THEN 'it needs something in another schema' "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'n' "
"      AND NOT (d.refclassid IN ('pg_type'::regclass, 'pg_proc'::regclass, 'pg_language'::regclass, 'pg_collation'::regclass, 'pg_operator'::regclass, 'pg_opclass'::regclass, 'pg_opfamily'::regclass) "
"        OR (d.refclassid = 'pg_namespace'::regclass AND d.refobjid = n.oid) "
"        OR (d.refclassid = 'pg_class'::regclass AND EXISTS (SELECT 1 FROM pg_class q WHERE q.oid = d.refobjid "
"          AND q.relkind IN ('r', 'p', 'v', 'm', 'c', 'S'))) "
"        OR (d.refclassid = 'pg_constraint'::regclass AND EXISTS (SELECT 1 FROM pg_constraint k WHERE k.oid = d.refobjid AND k.conrelid <> 0)) "
"        OR coalesce((SELECT i.schema = 'pg_catalog' FROM pg_identify_object(d.refclassid, d.refobjid, 0) i), false))) "
"    THEN 'it needs an object no commit holds yet' "
"  END " FUNCTIONS_OF_SCHEMA "ORDER BY 1";

/* Refuses a qualified name in a function's statement, or a path naming its schema. */
/*
 * Whether a body has a column with no name that would be given one if made again: a
 * constant no AS named prints with the cast that, read back, names it for its type, so
 * its text would differ.  (What is a column prints as its name, which read back is that.)
 */
static bool
unstable_column_walker(Node *node, void *arg)
{
	if (node == NULL)
		return false;
	if (IsA(node, ReturnStmt))
		return unstable_column_walker(((ReturnStmt *) node)->returnval, arg);
	if (IsA(node, ResTarget))
	{
		ResTarget  *rt = (ResTarget *) node;

		if (rt->name == NULL && rt->val != NULL && !IsA(rt->val, ColumnRef) && strcmp(FigureColname(rt->val), "?column?") != 0)
			return true;
	}
	return raw_expression_tree_walker(node, unstable_column_walker, arg);
}

static void
check_function_is_portable(const char *name, const char *schema, const char *tail)
{
	List	   *parsed = raw_parser(psprintf("CREATE FUNCTION x %s", tail), RAW_PARSE_DEFAULT);
	CreateFunctionStmt *f;
	NamesSchema n = {NULL, NULL};
	bool		named = false;
	ListCell   *lc;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CreateFunctionStmt))
		elog(ERROR, "function \"%s\" printed as something else", name);
	f = (CreateFunctionStmt *) linitial_node(RawStmt, parsed)->stmt;
	foreach(lc, f->parameters)
	{
		FunctionParameter *p = lfirst_node(FunctionParameter, lc);

		named = named || names_schema_walker((Node *) p->argType, &n) || names_schema_walker(p->defexpr, &n);
	}
	named = named || names_schema_walker((Node *) f->returnType, &n) || names_schema_walker(f->sql_body, &n);
	if (unstable_column_walker(f->sql_body, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: a column of its body has no name, and made again would be given one",
						name, schema),
				 errhint("Name the column with AS.")));
	foreach(lc, f->options)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "set") == 0 && IsA(d->arg, VariableSetStmt) &&
			pg_strcasecmp(((VariableSetStmt *) d->arg)->name, "search_path") == 0)
		{
			ListCell   *a;

			foreach(a, ((VariableSetStmt *) d->arg)->args)
			{
				Node	   *v = lfirst(a);

				if (IsA(v, A_Const) && IsA(&((A_Const *) v)->val, String) &&
					strcmp(strVal(&((A_Const *) v)->val), schema) == 0)
				{
					n.why = "it sets a search path that names its schema";
					named = true;
				}
			}
		}
	}
	if (named)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: %s", name, schema, n.why)));
}

/* Its owner, if it can travel, what is revoked from PUBLIC, and what it has granted. */
static void
read_function_owner_and_grants(WsObject *o, const char *oid, const char *name)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT r.rolname::text, p.proacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(p.proacl) e "
				"    WHERE e.grantee = 0 AND e.privilege_type = 'EXECUTE'), "
				"  NOT (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls) "
				"FROM pg_proc p JOIN pg_roles r ON r.oid = p.proowner WHERE p.oid = $1::oid", 1, params, &rows);
	if (rows.nrows != 1)
		printed_or_retry(name, NULL);
	if (ws_sql_value(&rows, 0, 2)[0] == 't' && !IsReservedName(ws_sql_value(&rows, 0, 0)))
		o->owner = role_in_commit(name, ws_sql_value(&rows, 0, 0));
	o->public_revoked = ws_sql_value(&rows, 0, 1)[0] == 't';
	ws_sql_rows_free(&rows);

	WsSql->read("SELECT r.rolname::text, e.is_grantable::text, "
				"  (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)::text "
				"FROM pg_proc p, aclexplode(p.proacl) e JOIN pg_roles r ON r.oid = e.grantee "
				"WHERE p.oid = $1::oid AND e.grantee <> p.proowner", 1, params, &rows);
	o->grants = zeroed(rows.nrows, sizeof(WsObjectGrant));
	o->ngrants = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		const char *grantee = ws_sql_value(&rows, i, 0);

		if (IsReservedName(grantee) || ws_sql_value(&rows, i, 2)[0] == 't')
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" cannot be held in a commit: it has a grant to role \"%s\", which cannot travel",
							name, grantee)));
		o->grants[i].grantee = role_in_commit(name, grantee);
		o->grants[i].grant_option = ws_sql_value(&rows, i, 1)[0] == 't';
		o->grants[i].privileges = px_memdup("X", 2);
	}
	if (o->ngrants > 1)
		qsort(o->grants, o->ngrants, sizeof(WsObjectGrant), grant_cmp);
	ws_sql_rows_free(&rows);
}

/* Adds the functions of a schema to objs, of *n and room for it. */
static WsObject *
read_functions(const char *schema, WsObject *objs, int *n)
{
	const char *params[1] = {schema};
	WsSqlRows	funcs;

	WsSql->read("SELECT p.oid::text, p.proname::text, p.prokind::text "
				FUNCTIONS_OF_SCHEMA, 1, params, &funcs);
	objs = repalloc0_array(objs, WsObject, Max(*n, 1), Max(*n + funcs.nrows, 1));
	for (int i = 0; i < funcs.nrows; i++)
	{
		WsObject   *o = &objs[(*n)++];
		const char *oid = ws_sql_value(&funcs, i, 0);
		const char *name = ws_sql_value(&funcs, i, 1);
		bool		procedure = ws_sql_value(&funcs, i, 2)[0] == PROKIND_PROCEDURE;
		char	   *printed = printed_or_retry(schema, ws_schema_print(schema, pg_get_functiondef, 1,
																	   ObjectIdGetDatum(atooid(oid)), 0, 0));
		char	   *head = psprintf("CREATE OR REPLACE %s %s", procedure ? "PROCEDURE" : "FUNCTION",
									quote_qualified_identifier(schema, name));
		size_t		len;
		char	   *tail;
		Buf			stored = BUF_INIT;
		const char *self_params[1] = {oid};
		WsSqlRows	rows;

		CHECK_FOR_INTERRUPTS();
		if (strncmp(printed, head, strlen(head)) != 0)
			elog(ERROR, "function \"%s\" printed as something else", name);
		tail = printed + strlen(head);
		len = strlen(tail);
		while (len > 0 && (tail[len - 1] == '\n' || tail[len - 1] == ' '))
			tail[--len] = '\0';
		check_function_is_portable(name, schema, tail);
		o->kind = procedure ? WS_OBJ_PROCEDURE : WS_OBJ_FUNCTION;
		o->name = ws_name_to_utf8(name);
		o->args = ws_text_to_utf8(function_args(schema, atooid(oid), NULL));
		ws_tags_escape(ws_text_to_utf8(tail), &stored);
		buf_append_byte(&stored, 0);
		o->definition = px_memdup(stored.data, stored.len);
		buf_free(&stored);
		check_constants_hold_no_names(name, schema, oid,
									  /* (what a default has, and a body's other than its arguments' and result) */
									  "SELECT a::text FROM pg_proc p, unnest(p.proargtypes::oid[]) WITH ORDINALITY u(a, i) "
									  "WHERE p.oid = $1::oid AND i > p.pronargs - p.pronargdefaults "
									  "UNION SELECT d.refobjid::text FROM pg_proc p JOIN pg_depend d ON d.classid = 'pg_proc'::regclass "
									  "AND d.objid = p.oid WHERE p.oid = $1::oid AND d.deptype = 'n' AND d.refclassid = 'pg_type'::regclass "
									  "AND p.prosqlbody IS NOT NULL AND d.refobjid <> p.prorettype AND NOT d.refobjid = ANY (p.proargtypes::oid[])");
		read_deps(o, schema, oid, "SELECT 'pg_proc'::regclass::oid, $1::oid");

		/* (it needs a table if a body or a type of it does) */
		WsSql->read("SELECT 1 FROM pg_depend d LEFT JOIN pg_type t ON d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid "
					"LEFT JOIN pg_type e ON e.oid = t.typelem AND e.typarray = t.oid "
					"JOIN pg_class q ON q.oid = CASE WHEN d.refclassid = 'pg_class'::regclass THEN d.refobjid ELSE coalesce(e.typrelid, t.typrelid) END "
					"WHERE d.classid = 'pg_proc'::regclass AND d.objid = $1::oid AND d.deptype = 'n' AND (q.relkind NOT IN ('c', 'S') "
					"  OR EXISTS (SELECT 1 FROM pg_depend i WHERE i.classid = 'pg_class'::regclass AND i.objid = q.oid AND i.deptype = 'i' AND i.refclassid = 'pg_class'::regclass)) LIMIT 1",
					1, self_params, &rows);
		o->after_tables = rows.nrows > 0;
		ws_sql_rows_free(&rows);

		WsSql->read("SELECT NULL::text, description FROM pg_description WHERE classoid = 'pg_proc'::regclass AND objoid = $1::oid",
					1, self_params, &rows);
		o->comments = zeroed(rows.nrows, sizeof(WsObjectComment));
		o->ncomments = rows.nrows;
		for (int c = 0; c < rows.nrows; c++)
			o->comments[c].text = ws_text_to_utf8(ws_sql_value(&rows, c, 1));
		ws_sql_rows_free(&rows);
		read_function_owner_and_grants(o, oid, name);
		pfree(head);
	}
	ws_sql_rows_free(&funcs);
	return objs;
}

/*
 * A function that needs what is made after tables is too, and a type cannot be, as
 * tables may need it.  Nor may a table need such a function.
 */
static void
settle_phases(const char *schema, WsObject *objs, int n)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;
	bool		changed = true;

	while (changed)
	{
		changed = false;
		for (int i = 0; i < n; i++)
			if (ws_obj_before_tables(&objs[i]))
				for (int d = 0; d < objs[i].ndeps; d++)
				{
					int			of = find_object(objs, n, objs[i].deps[d]);

					if (of < 0 || ws_obj_before_tables(&objs[of]))
						continue;
					if (!is_function(objs[i].kind))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: it needs \"%s\", which needs a table, and a table may need it",
										objs[i].name, schema, objs[of].name)));
					objs[i].after_tables = true;
					changed = true;
				}
	}
	WsSql->read("WITH src(classid, objid) AS ( "
				"  SELECT 'pg_attrdef'::regclass::oid, a.oid FROM pg_attrdef a JOIN pg_class c ON c.oid = a.adrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind IN ('r', 'p') "
				"  UNION ALL SELECT 'pg_constraint'::regclass::oid, k.oid FROM pg_constraint k JOIN pg_class c ON c.oid = k.conrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind IN ('r', 'p') "
				"  UNION ALL SELECT 'pg_class'::regclass::oid, i.indexrelid FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind IN ('r', 'p') "
				"  UNION ALL SELECT 'pg_class'::regclass::oid, c.oid FROM pg_class c "
				"    JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name AND c.relkind IN ('r', 'p')) "
				"SELECT DISTINCT d.refobjid::text FROM src JOIN pg_depend d ON d.classid = src.classid AND d.objid = src.objid "
				"JOIN pg_proc p ON d.refclassid = 'pg_proc'::regclass AND p.oid = d.refobjid "
				"JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = $1::name", 1, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
	{
		Buf			key = BUF_INIT;
		int			of;

		function_key(schema, atooid(ws_sql_value(&rows, i, 0)), &key);
		buf_append_byte(&key, 0);
		of = find_object(objs, n, (char *) key.data);
		if (of >= 0 && !ws_obj_before_tables(&objs[of]))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("\"%s\" in schema \"%s\" cannot be held in a commit: a table needs it, and it needs a table",
							objs[of].name, schema)));
		buf_free(&key);
	}
	ws_sql_rows_free(&rows);
}

void
ws_obj_read(const char *schema, WsObject **out, int *n)
{
	int			level = WsSql->begin();
	const char *params[1] = {schema};
	WsSqlRows	rels;
	WsObject   *objs;
	int			total;
	char	   *version;

	ws_schema_check_usage(schema);
	version = push_locked_snapshot(schema);
	refuse_what_cannot_be_held(schema);
	refuse_types(schema, type_refusal_sql);
	refuse_types(schema, cast_refusal_sql);
	refuse_types(schema, range_cast_refusal_sql);
	refuse_types(schema, function_refusal_sql);
	refuse_types(schema, sequence_refusal_sql);
	refuse_types(schema, trigger_refusal_sql);
	refuse_types(schema, trigger_clone_refusal_sql);
	refuse_types(schema, rule_refusal_sql);
	refuse_types(schema, policy_refusal_sql);
	refuse_types(schema, collation_refusal_sql);
	refuse_types(schema, conversion_refusal_sql);
	refuse_types(schema, operator_refusal_sql);
	refuse_types(schema, opfamily_refusal_sql);
	refuse_types(schema, opclass_refusal_sql);
	refuse_types(schema, member_refusal_sql);
	refuse_types(schema, index_refusal_sql);
	refuse_types(schema, default_refusal_sql);

	WsSql->read("SELECT c.oid::text, c.relkind::text, c.relname::text, NULL::text, "
				"  (SELECT string_agg(quote_ident(split_part(o, '=', 1)) || ' = ' || "
				"      quote_literal(substr(o, strpos(o, '=') + 1)), ', ' ORDER BY o) FROM unnest(c.reloptions) o) "
				"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE n.nspname = $1::name AND c.relkind IN ('v', 'm')", 1, params, &rels);
	objs = palloc0_array(WsObject, Max(rels.nrows, 1));
	for (int i = 0; i < rels.nrows; i++)
	{
		WsObject   *o = &objs[i];
		const char *oid = ws_sql_value(&rels, i, 0);
		bool		matview = ws_sql_value(&rels, i, 1)[0] == 'm';
		const char *name = ws_sql_value(&rels, i, 2);
		char	   *viewdef = printed_or_retry(schema, ws_schema_print(schema, pg_get_viewdef_ext, 2,
																	   ObjectIdGetDatum(atooid(oid)),
																	   BoolGetDatum(false), 0));
		const char *options = ws_sql_value(&rels, i, 4);
		Buf			text = BUF_INIT;
		Buf			stored = BUF_INIT;
		char	   *body;
		char	   *trimmed;
		size_t		len;

		CHECK_FOR_INTERRUPTS();
		o->kind = matview ? WS_OBJ_MATVIEW : WS_OBJ_VIEW;
		o->name = ws_name_to_utf8(name);
		if (options != NULL)
		{
			ws_put_text(&text, "WITH (");
			ws_put_text(&text, options);
			ws_put_text(&text, ") ");
		}
		ws_put_text(&text, "AS ");
		/* (pg_get_viewdef ends with a semicolon, which a matview's WITH NO DATA follows) */
		trimmed = pstrdup(viewdef);
		len = strlen(trimmed);
		while (len > 0 && (trimmed[len - 1] == ';' || trimmed[len - 1] == ' ' || trimmed[len - 1] == '\n'))
			trimmed[--len] = '\0';
		check_query_is_portable(name, schema, trimmed);
		ws_put_text(&text, trimmed);
		pfree(trimmed);
		if (matview)
			ws_put_text(&text, " WITH NO DATA");
		buf_append_byte(&text, 0);
		body = ws_text_to_utf8((char *) text.data);
		ws_tags_escape(body, &stored);
		buf_append_byte(&stored, 0);
		o->definition = px_memdup(stored.data, stored.len);
		buf_free(&stored);
		buf_free(&text);
		check_constants_hold_no_names(name, schema, oid,
									  "SELECT d.refobjid::text FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass "
									  "AND d.objid = r.oid WHERE r.ev_class = $1::oid AND r.rulename = '_RETURN' AND d.deptype = 'n' AND d.refclassid = 'pg_type'::regclass");
		read_deps(o, schema, oid, "SELECT 'pg_rewrite'::regclass::oid, oid FROM pg_rewrite WHERE ev_class = $1::oid AND rulename = '_RETURN'");
		read_comments(o, oid);
		read_owner_and_grants(o, oid, name, true);
	}
	total = rels.nrows;
	objs = read_types(schema, objs, &total);
	objs = read_functions(schema, objs, &total);
	objs = read_sequences(schema, objs, &total);
	objs = read_collations(schema, objs, &total);
	objs = read_conversions(schema, objs, &total);
	objs = read_casts(schema, objs, &total);
	objs = read_operators(schema, objs, &total);
	objs = read_opfamilies(schema, objs, &total);
	objs = read_opclasses(schema, objs, &total);
	objs = read_of_table(schema, WS_OBJ_TRIGGER, objs, &total);
	objs = read_of_table(schema, WS_OBJ_RULE, objs, &total);
	objs = read_of_table(schema, WS_OBJ_POLICY, objs, &total);
	objs = read_of_table(schema, WS_OBJ_INDEX, objs, &total);
	objs = read_of_table(schema, WS_OBJ_DEFAULT, objs, &total);
	if (total > 1)
		qsort(objs, total, sizeof(WsObject), object_cmp);
	settle_phases(schema, objs, total);
	{
		/* (views can be made to need each other, which no restore could make) */
		int		   *order = palloc_array(int, Max(total, 1));

		if (!compute_order(objs, total, order))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("schema \"%s\" cannot be held in a commit: its objects need each other in a cycle", schema)));
		pfree(order);
	}
	*n = total;
	*out = objs;
	ws_sql_rows_free(&rels);
	PopActiveSnapshot();
	INJECTION_POINT("ws-obj-read-end", NULL);

	/* (what prints names reads them as they are now, however long this has taken) */
	CommandCounterIncrement();
	PushActiveSnapshot(GetLatestSnapshot());
	UpdateActiveSnapshotCommandId();
	if (strcmp(version, relations_version(schema)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not read schema \"%s\": it changed while it was being read", schema)));
	PopActiveSnapshot();
	WsSql->end(level);
}

/* ---------------------------------------------------------------- the map */

static int
kv_cmp(const void *a, const void *b)
{
	const ProllyKV *x = a;
	const ProllyKV *y = b;
	int			c = memcmp(x->key, y->key, Min(x->klen, y->klen));

	return c != 0 ? c : (x->klen < y->klen ? -1 : x->klen > y->klen);
}

void
ws_obj_map(ProllyCtx *ctx, const WsObject *objs, int n, PHash *map)
{
	ProllyKV   *kvs = palloc_array(ProllyKV, Max(n, 1));
	Buf		   *keys = palloc0_array(Buf, Max(n, 1));
	Buf		   *vals = palloc0_array(Buf, Max(n, 1));
	int			rc;

	for (int i = 0; i < n; i++)
	{
		WsObjectCodec->encode_key(&objs[i], &keys[i]);
		WsObjectCodec->encode_value(&objs[i], &vals[i]);
		kvs[i].key = keys[i].data;
		kvs[i].klen = keys[i].len;
		kvs[i].val = vals[i].data;
		kvs[i].vlen = vals[i].len;
	}
	if (n > 1)
		qsort(kvs, n, sizeof(ProllyKV), kv_cmp);
	for (int i = 1; i < n; i++)
		if (kv_cmp(&kvs[i - 1], &kvs[i]) == 0)
			elog(ERROR, "two objects of one key");
	rc = prolly_build(ctx, 0, kvs, n, map);
	if (rc < 0)
		prolly_report(ctx);
	for (int i = 0; i < n; i++)
	{
		buf_free(&keys[i]);
		buf_free(&vals[i]);
	}
	pfree(kvs);
	pfree(keys);
	pfree(vals);
}

void
ws_seq_map(ProllyCtx *ctx, const WsSeqPosition *positions, int n, PHash *map)
{
	ProllyKV   *kvs = palloc_array(ProllyKV, Max(n, 1));
	Buf		   *vals = palloc0_array(Buf, Max(n, 1));
	int			rc;

	for (int i = 0; i < n; i++)
	{
		ws_seq_encode(positions[i].last_value, positions[i].is_called, &vals[i]);
		kvs[i].key = (const uint8 *) positions[i].name;
		kvs[i].klen = strlen(positions[i].name);
		kvs[i].val = vals[i].data;
		kvs[i].vlen = vals[i].len;
	}
	if (n > 1)
		qsort(kvs, n, sizeof(ProllyKV), kv_cmp);
	for (int i = 1; i < n; i++)
		if (kv_cmp(&kvs[i - 1], &kvs[i]) == 0)
			elog(ERROR, "two sequences of one name");
	rc = prolly_build(ctx, 0, kvs, n, map);
	if (rc < 0)
		prolly_report(ctx);
	for (int i = 0; i < n; i++)
		buf_free(&vals[i]);
	pfree(kvs);
	pfree(vals);
}

void
ws_seq_list(ProllyCtx *ctx, const PHash *map, WsSeqPosition **out, int *n)
{
	ProllyIter *it;
	const uint8 *key;
	const uint8 *val;
	size_t		klen;
	size_t		vlen;
	int			rc;
	int			cap = 8;
	int			i = 0;

	*out = palloc0_array(WsSeqPosition, cap);
	rc = prolly_iter_open(ctx, map, NULL, 0, &it);
	if (rc < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		if (i == cap)
		{
			*out = repalloc0_array(*out, WsSeqPosition, cap, cap * 2);
			cap *= 2;
		}
		(*out)[i].name = pnstrdup((const char *) key, klen);
		ws_seq_decode(val, vlen, &(*out)[i].last_value, &(*out)[i].is_called);
		i++;
	}
	if (rc < 0)
		prolly_report(ctx);
	prolly_iter_close(it);
	*n = i;
}

void
ws_obj_list(ProllyCtx *ctx, const PHash *map, WsObject **out, int *n)
{
	ProllyIter *it;
	const uint8 *key;
	const uint8 *val;
	size_t		klen;
	size_t		vlen;
	int			rc;
	int			cap = 8;
	int			i = 0;

	/* (grown as they are read: a root's count is not to be trusted with memory) */
	*out = palloc0_array(WsObject, cap);
	rc = prolly_iter_open(ctx, map, NULL, 0, &it);
	if (rc < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		if (i == cap)
		{
			*out = repalloc0_array(*out, WsObject, cap, cap * 2);
			cap *= 2;
		}
		WsObjectCodec->decode(key, klen, val, vlen, &(*out)[i++]);
	}
	if (rc < 0)
		prolly_report(ctx);
	prolly_iter_close(it);
	*n = i;
}

/* ---------------------------------------------------------------- restore */

void
ws_obj_restore_order(const WsObject *objs, int n, int *order)
{
	if (!compute_order(objs, n, order))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a commit's objects need each other in a cycle")));
}

pg_noreturn static void
not_the_object(const WsObject *o, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("object \"%s\" does not make that object: %s", o->name, what)));
}

static const char *
no_name(void *arg, uint64 tag)
{
	return NULL;
}

/* The text as parse trees, or a refusal: it is the record's fault. */
static List *
parse_object_text(const WsObject *o, const char *sql)
{
	MemoryContext cxt = CurrentMemoryContext;
	List	   *volatile parsed = NIL;

	PG_TRY();
	{
		parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION &&
			ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_FEATURE_NOT_SUPPORTED)
			PG_RE_THROW();
		FlushErrorState();
		not_the_object(o, edata->message);
	}
	PG_END_TRY();
	return parsed;
}

/* Whether a list of names is just this schema and name. */
static bool
names_it(List *names, const char *schema, const char *name)
{
	return list_length(names) == 2 && strcmp(strVal(linitial(names)), schema) == 0 &&
		strcmp(strVal(lsecond(names)), name) == 0;
}

void
ws_obj_create_statement(const WsObject *o, const char *schema, Buf *out)
{
	const char *problem = ws_obj_problem(o);
	char	   *here;
	char	   *definition;
	Buf			sql = BUF_INIT;
	uint64		missing;
	List	   *parsed;
	Node	   *node;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	here = name_here(o, o->name);
	definition = ws_text_from_utf8(o->definition);
	if (!is_whole_statement(o->kind))
	{
		ws_put_text(&sql, "CREATE ");
		ws_put_text(&sql, kind_sql[o->kind]);
		ws_put_text(&sql, " ");
		ws_put_identifier(&sql, schema);
		ws_put_text(&sql, ".");
		if (o->kind == WS_OBJ_OPERATOR)
			ws_put_text(&sql, here);
		else
			ws_put_identifier(&sql, here);
		ws_put_text(&sql, " ");
	}
	if (!ws_tags_render(definition, no_name, NULL, &sql, &missing))
		not_the_object(o, "a reference to a column");
	buf_append_byte(&sql, 0);
	parsed = parse_object_text(o, (char *) sql.data);
	node = list_length(parsed) == 1 ? linitial_node(RawStmt, parsed)->stmt : NULL;
	switch (o->kind)
	{
		case WS_OBJ_VIEW:
			{
				ViewStmt   *v = (ViewStmt *) node;

				if (node == NULL || !IsA(node, ViewStmt))
					not_the_object(o, "it is not one CREATE VIEW");
				if (v->replace || v->view->schemaname == NULL || strcmp(v->view->schemaname, schema) != 0 ||
					v->view->catalogname != NULL || strcmp(v->view->relname, here) != 0 || v->view->relpersistence != RELPERSISTENCE_PERMANENT ||
					v->aliases != NIL)
					not_the_object(o, "another view");
				break;
			}
		case WS_OBJ_FUNCTION:
		case WS_OBJ_PROCEDURE:
			{
				CreateFunctionStmt *f = (CreateFunctionStmt *) node;

				if (node == NULL || !IsA(node, CreateFunctionStmt))
					not_the_object(o, "it is not one CREATE FUNCTION or PROCEDURE");
				if (f->replace || f->is_procedure != (o->kind == WS_OBJ_PROCEDURE) ||
					!names_it(f->funcname, schema, here))
					not_the_object(o, "another function");
				break;
			}
		case WS_OBJ_TRIGGER:
		case WS_OBJ_RULE:
		case WS_OBJ_POLICY:
		case WS_OBJ_INDEX:
		case WS_OBJ_DEFAULT:
			{
				char	   *table = name_here(o, o->args);
				RangeVar   *on = NULL;
				const char *made = NULL;

				if (node != NULL && o->kind == WS_OBJ_TRIGGER && IsA(node, CreateTrigStmt) &&
					!((CreateTrigStmt *) node)->replace)
				{
					on = ((CreateTrigStmt *) node)->relation;
					made = ((CreateTrigStmt *) node)->trigname;
				}
				else if (node != NULL && o->kind == WS_OBJ_RULE && IsA(node, RuleStmt) && !((RuleStmt *) node)->replace)
				{
					on = ((RuleStmt *) node)->relation;
					made = ((RuleStmt *) node)->rulename;
				}
				else if (node != NULL && o->kind == WS_OBJ_POLICY && IsA(node, CreatePolicyStmt))
				{
					on = ((CreatePolicyStmt *) node)->table;
					made = ((CreatePolicyStmt *) node)->policy_name;
				}
				else if (node != NULL && o->kind == WS_OBJ_INDEX && IsA(node, IndexStmt) &&
						 !((IndexStmt *) node)->concurrent && !((IndexStmt *) node)->if_not_exists &&
						 ((IndexStmt *) node)->tableSpace == NULL)
				{
					on = ((IndexStmt *) node)->relation;
					made = ((IndexStmt *) node)->idxname;
				}
				else if (node != NULL && o->kind == WS_OBJ_DEFAULT && IsA(node, AlterTableStmt) &&
						 ((AlterTableStmt *) node)->objtype == OBJECT_VIEW && !((AlterTableStmt *) node)->missing_ok &&
						 list_length(((AlterTableStmt *) node)->cmds) == 1 &&
						 linitial_node(AlterTableCmd, ((AlterTableStmt *) node)->cmds)->subtype == AT_ColumnDefault &&
						 linitial_node(AlterTableCmd, ((AlterTableStmt *) node)->cmds)->def != NULL)
				{
					on = ((AlterTableStmt *) node)->relation;
					made = linitial_node(AlterTableCmd, ((AlterTableStmt *) node)->cmds)->name;
				}
				if (on == NULL || made == NULL || !on->inh || on->schemaname != NULL || on->catalogname != NULL ||
					strcmp(on->relname, table) != 0 || strcmp(made, here) != 0)
					not_the_object(o, "it is not one statement making it, on its table");
				pfree(table);
				break;
			}
		case WS_OBJ_OPERATOR:
			if (node == NULL || !IsA(node, DefineStmt) || ((DefineStmt *) node)->kind != OBJECT_OPERATOR ||
				!names_it(((DefineStmt *) node)->defnames, schema, here))
				not_the_object(o, "it is not one CREATE OPERATOR of it");
			break;
		case WS_OBJ_OPLINK:
			if (node == NULL || !IsA(node, AlterOperatorStmt))
				not_the_object(o, "it is not one ALTER OPERATOR");
			break;
		case WS_OBJ_OPCLASS:
			if (node == NULL || !IsA(node, CreateOpClassStmt) || !names_it(((CreateOpClassStmt *) node)->opclassname, schema, here))
				not_the_object(o, "it is not one CREATE OPERATOR CLASS of it");
			break;
		case WS_OBJ_OPFAMILY:
			if (node == NULL || !IsA(node, CreateOpFamilyStmt) || !names_it(((CreateOpFamilyStmt *) node)->opfamilyname, schema, here))
				not_the_object(o, "it is not one CREATE OPERATOR FAMILY of it");
			break;
		case WS_OBJ_OPMEMBERS:
			if (node == NULL || !IsA(node, AlterOpFamilyStmt) || ((AlterOpFamilyStmt *) node)->isDrop)
				not_the_object(o, "it is not one ALTER OPERATOR FAMILY ADD");
			break;
		case WS_OBJ_CAST:
			if (node == NULL || !IsA(node, CreateCastStmt))
				not_the_object(o, "it is not one CREATE CAST");
			break;
		case WS_OBJ_COLLATION:
			if (node == NULL || !IsA(node, DefineStmt) || ((DefineStmt *) node)->kind != OBJECT_COLLATION ||
				((DefineStmt *) node)->if_not_exists || !names_it(((DefineStmt *) node)->defnames, schema, here))
				not_the_object(o, "it is not one CREATE COLLATION of it");
			break;
		case WS_OBJ_CONVERSION:
			if (node == NULL || !IsA(node, CreateConversionStmt) || ((CreateConversionStmt *) node)->def ||
				!names_it(((CreateConversionStmt *) node)->conversion_name, schema, here))
				not_the_object(o, "it is not one CREATE CONVERSION of it");
			break;
		case WS_OBJ_SEQUENCE:
			{
				CreateSeqStmt *q = (CreateSeqStmt *) node;
				ListCell   *lc;

				if (node == NULL || !IsA(node, CreateSeqStmt))
					not_the_object(o, "it is not one CREATE SEQUENCE");
				if (q->if_not_exists || q->for_identity || q->sequence->schemaname == NULL ||
					strcmp(q->sequence->schemaname, schema) != 0 || q->sequence->catalogname != NULL ||
					strcmp(q->sequence->relname, here) != 0 ||
					q->sequence->relpersistence != RELPERSISTENCE_PERMANENT)
					not_the_object(o, "another sequence");
				foreach(lc, q->options)
					if (strcmp(lfirst_node(DefElem, lc)->defname, "owned_by") == 0)
						not_the_object(o, "one owned there, where its column is not yet");
				break;
			}
		case WS_OBJ_DOMAIN:
			{
				CreateDomainStmt *d = (CreateDomainStmt *) node;

				if (node == NULL || !IsA(node, CreateDomainStmt))
					not_the_object(o, "it is not one CREATE DOMAIN");
				if (!names_it(d->domainname, schema, here))
					not_the_object(o, "another domain");
				break;
			}
		case WS_OBJ_TYPE:
			{
				List	   *names = NIL;

				if (node != NULL && IsA(node, CreateEnumStmt))
					names = ((CreateEnumStmt *) node)->typeName;
				else if (node != NULL && IsA(node, CreateRangeStmt))
					names = ((CreateRangeStmt *) node)->typeName;
				else if (node != NULL && IsA(node, CompositeTypeStmt))
				{
					RangeVar   *v = ((CompositeTypeStmt *) node)->typevar;

					if (v->catalogname != NULL || v->schemaname == NULL)
						not_the_object(o, "another type");
					names = list_make2(makeString(v->schemaname), makeString(v->relname));
				}
				else
					not_the_object(o, "it is not one CREATE TYPE of a kind held");
				if (!names_it(names, schema, here))
					not_the_object(o, "another type");
				break;
			}
		case WS_OBJ_MATVIEW:
			{
				CreateTableAsStmt *m = (CreateTableAsStmt *) node;

				if (node == NULL || !IsA(node, CreateTableAsStmt) || m->objtype != OBJECT_MATVIEW)
					not_the_object(o, "it is not one CREATE MATERIALIZED VIEW");
				if (m->if_not_exists || m->into->rel->schemaname == NULL ||
					strcmp(m->into->rel->schemaname, schema) != 0 || m->into->rel->catalogname != NULL ||
					strcmp(m->into->rel->relname, here) != 0 ||
					m->into->rel->relpersistence != RELPERSISTENCE_PERMANENT ||
					m->into->colNames != NIL || m->into->accessMethod != NULL ||
					m->into->tableSpaceName != NULL || !m->into->skipData)
					not_the_object(o, "another materialized view, or one with data");
				break;
			}
	}
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
	pfree(definition);
	pfree(here);
}

static void
put_object(const WsObject *o, const char *schema, Buf *sql)
{
	char	   *here = name_here(o, o->name);

	ws_put_identifier(sql, schema);
	ws_put_text(sql, ".");
	if (o->kind == WS_OBJ_OPERATOR)
		ws_put_text(sql, here);
	else
		ws_put_identifier(sql, here);
	pfree(here);
	if (o->kind == WS_OBJ_OPCLASS || o->kind == WS_OBJ_OPFAMILY)
	{
		char	   *method = ws_text_from_utf8(o->args);

		ws_put_text(sql, " USING ");
		ws_put_text(sql, method);
		pfree(method);
	}
	else if (o->args != NULL)
	{
		char	   *args = ws_text_from_utf8(o->args);

		ws_put_text(sql, "(");
		ws_put_text(sql, args);
		ws_put_text(sql, ")");
		pfree(args);
	}
}

void
ws_obj_comment_statement(const WsObject *o, int k, const char *schema, Buf *out)
{
	const WsObjectComment *c;
	char	   *text;

	if (k < 0 || k >= o->ncomments)
		elog(ERROR, "no comment %d", k);
	c = &o->comments[k];
	ws_put_text(out, "COMMENT ON ");
	if (o->kind == WS_OBJ_CAST)
	{
		char	   *source = ws_text_from_utf8(o->name);
		char	   *target = ws_text_from_utf8(o->args);

		ws_put_text(out, "CAST (");
		ws_put_text(out, source);
		ws_put_text(out, " AS ");
		ws_put_text(out, target);
		ws_put_text(out, ") IS ");
		text = ws_text_from_utf8(c->text);
		ws_put_text(out, quote_literal_cstr(text));
		pfree(text);
		pfree(source);
		pfree(target);
		return;
	}
	if (o->kind == WS_OBJ_INDEX)
	{
		char	   *here = name_here(o, o->name);

		ws_put_text(out, "INDEX ");
		ws_put_identifier(out, schema);
		ws_put_text(out, ".");
		ws_put_identifier(out, here);
		ws_put_text(out, " IS ");
		text = ws_text_from_utf8(c->text);
		ws_put_text(out, quote_literal_cstr(text));
		pfree(text);
		pfree(here);
		return;
	}
	if (is_of_table(o->kind))
	{
		char	   *here = name_here(o, o->name);
		char	   *table = name_here(o, o->args);

		ws_put_text(out, kind_sql[o->kind]);
		ws_put_text(out, " ");
		ws_put_identifier(out, here);
		ws_put_text(out, " ON ");
		ws_put_identifier(out, schema);
		ws_put_text(out, ".");
		ws_put_identifier(out, table);
		ws_put_text(out, " IS ");
		text = ws_text_from_utf8(c->text);
		ws_put_text(out, quote_literal_cstr(text));
		pfree(text);
		pfree(here);
		pfree(table);
		return;
	}
	if (c->column != NULL)
		ws_put_text(out, "COLUMN ");
	else
	{
		ws_put_text(out, kind_sql[o->kind]);
		ws_put_text(out, " ");
	}
	put_object(o, schema, out);
	if (c->column != NULL)
	{
		char	   *column = ws_name_from_utf8(c->column);

		ws_put_text(out, ".");
		ws_put_identifier(out, column);
		pfree(column);
	}
	ws_put_text(out, " IS ");
	text = ws_text_from_utf8(c->text);
	ws_put_text(out, quote_literal_cstr(text));
	pfree(text);
}

void
ws_obj_owner_statement(const WsObject *o, const char *schema, Buf *out)
{
	char	   *owner;
	char	   *what;
	const char *problem;

	if (o->owner == NULL)
		return;
	owner = ws_name_from_utf8(o->owner);
	what = psprintf("object \"%s\"", o->name);
	ws_role_check_name(what, owner);
	pfree(what);
	problem = ws_role_travel_problem(owner);
	if (problem != NULL)
		not_the_object(o, "a role that can log in, or with powers no commit holds");
	ws_put_text(out, "ALTER ");
	ws_put_text(out, kind_sql[o->kind]);
	ws_put_text(out, " ");
	put_object(o, schema, out);
	ws_put_text(out, " OWNER TO ");
	ws_put_identifier(out, owner);
	pfree(owner);
}

/* The ALTER TABLE that makes a trigger or rule fire as it is held, or nothing. */
static void
fires_statement(const WsObject *o, const char *schema, Buf *out)
{
	char	   *here;
	char	   *table;

	if (o->fires == 0)
		return;
	here = name_here(o, o->name);
	table = name_here(o, o->args);
	ws_put_text(out, "ALTER TABLE ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, table);
	ws_put_text(out, o->fires == 'D' ? " DISABLE " : o->fires == 'R' ? " ENABLE REPLICA " : " ENABLE ALWAYS ");
	ws_put_text(out, o->kind == WS_OBJ_TRIGGER ? "TRIGGER " : "RULE ");
	ws_put_identifier(out, here);
	pfree(here);
	pfree(table);
}

static void
add_role(char ***names, int *n, const char *role)
{
	*names = *names == NULL ? palloc_array(char *, 1) : repalloc_array(*names, char *, *n + 1);
	(*names)[(*n)++] = pstrdup(role);
}

void
ws_obj_roles(const WsObject *o, char ***names, int *n)
{
	if (o->owner != NULL)
		add_role(names, n, o->owner);
	for (int i = 0; i < o->ngrants; i++)
		if (o->grants[i].grantee != NULL)
			add_role(names, n, o->grants[i].grantee);
	if (o->kind == WS_OBJ_POLICY)
	{
		Buf			sql = BUF_INIT;
		uint64		missing;
		List	   *parsed;
		CreatePolicyStmt *stmt;
		ListCell   *lc;

		if (!ws_tags_render(o->definition, no_name, NULL, &sql, &missing))
			not_the_object(o, "a reference to a column");
		buf_append_byte(&sql, 0);
		parsed = parse_object_text(o, (char *) sql.data);
		if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CreatePolicyStmt))
			not_the_object(o, "it is not one CREATE POLICY");
		stmt = (CreatePolicyStmt *) linitial_node(RawStmt, parsed)->stmt;
		foreach(lc, stmt->roles)
		{
			RoleSpec   *r = lfirst_node(RoleSpec, lc);

			if (r->roletype == ROLESPEC_CSTRING)
				add_role(names, n, r->rolename);
		}
		buf_free(&sql);
	}
}

void
ws_obj_owned_statement(const WsObject *o, const char *schema, Buf *out)
{
	char	   *table;
	char	   *column;

	if (o->owned_table == NULL)
		return;
	table = ws_name_from_utf8(o->owned_table);
	column = ws_name_from_utf8(o->owned_column);
	ws_put_text(out, "ALTER SEQUENCE ");
	put_object(o, schema, out);
	ws_put_text(out, " OWNED BY ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, table);
	ws_put_text(out, ".");
	ws_put_identifier(out, column);
	pfree(table);
	pfree(column);
}

void
ws_obj_grant_statement(const WsObject *o, int k, const char *schema, Buf *out)
{
	const WsObjectGrant *g;
	char	   *column = NULL;

	if (k < 0 || k >= o->ngrants)
		elog(ERROR, "no grant %d", k);
	g = &o->grants[k];
	if (g->column != NULL)
		column = ws_name_from_utf8(g->column);
	ws_put_text(out, "GRANT ");
	for (const char *l = g->privileges; *l; l++)
	{
		if (l > g->privileges)
			ws_put_text(out, ", ");
		ws_put_text(out, ws_privilege_word(*l));
		if (column != NULL)
		{
			ws_put_text(out, " (");
			ws_put_identifier(out, column);
			ws_put_text(out, ")");
		}
	}
	ws_put_text(out, " ON ");
	ws_put_text(out, is_relation(o->kind) ? "TABLE" : is_function(o->kind) ? "ROUTINE" : kind_sql[o->kind]);
	ws_put_text(out, " ");
	put_object(o, schema, out);
	ws_put_text(out, " TO ");
	if (g->grantee == NULL)
		ws_put_text(out, "PUBLIC");
	else
	{
		char	   *grantee = ws_name_from_utf8(g->grantee);
		char	   *what = psprintf("object \"%s\"", o->name);

		ws_role_check_name(what, grantee);
		pfree(what);
		if (ws_role_travel_problem(grantee) != NULL)
			not_the_object(o, "a role that can log in, or with powers no commit holds");
		/* (who restores owns it if no one is held, and a grant to its owner is none) */
		if (o->owner == NULL && strcmp(grantee, GetUserNameFromId(GetUserId(), false)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("object \"%s\" has a grant to role \"%s\", who would own it", o->name, grantee)));
		ws_put_identifier(out, grantee);
		pfree(grantee);
	}
	if (g->grant_option)
		ws_put_text(out, " WITH GRANT OPTION");
	if (column != NULL)
		pfree(column);
}

static void
run_statement(Buf *sql)
{
	WsSqlRows	rows;

	buf_append_byte(sql, 0);
	WsSql->run((char *) sql->data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	CommandCounterIncrement();
	buf_free(sql);
}

/*
 * Refuses a schema with a function or operator a definition's unqualified name could
 * be taken for: an exact match here is preferred to a polymorphic one in the catalogs,
 * though the catalogs come first.  So only one named as something in the catalogs can
 * be: all others, such as a range type's constructors or what a commit's tables use,
 * are nothing to a definition.  Types and collations are matched by name, so the
 * catalogs' come first.
 */
static void
check_nothing_captures(const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->run("SELECT what FROM (SELECT 'a function' AS what FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
			   "    WHERE n.nspname = $1::name AND EXISTS (SELECT 1 FROM pg_proc c WHERE c.proname = p.proname "
			   "      AND c.pronamespace = 'pg_catalog'::regnamespace) "
			   "  UNION ALL SELECT 'an operator' FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace "
			   "    WHERE n.nspname = $1::name AND EXISTS (SELECT 1 FROM pg_operator c WHERE c.oprname = o.oprname "
			   "      AND c.oprnamespace = 'pg_catalog'::regnamespace)) w LIMIT 1", 1, params, &rows);
	if (rows.nrows > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("schema \"%s\" holds %s named as one in the catalogs, which the commit's definitions could be taken for",
						schema, ws_sql_value(&rows, 0, 0))));
	ws_sql_rows_free(&rows);
}

/*
 * Refuses a collation a bare name would not mean: the catalogs come first on the path,
 * and what collations they hold is each server's, by its locales and library, so what
 * capture found free may not be free here.
 */
static void
check_collations_are_free(const WsObject *objs, int n, const char *schema)
{
	for (int i = 0; i < n; i++)
	{
		char	   *here;
		char	   *method;
		const char *params[2];
		WsSqlRows	rows;
		const char *sql;

		if (objs[i].kind == WS_OBJ_COLLATION)
			/* (one for another encoding is no name this database can mean) */
			sql = "SELECT 1 FROM pg_collation WHERE collname = $1::name AND collnamespace = 'pg_catalog'::regnamespace "
				"AND collencoding IN (-1, pg_char_to_encoding(getdatabaseencoding()))";
		else if (objs[i].kind == WS_OBJ_OPCLASS)
			sql = "SELECT 1 FROM pg_opclass o JOIN pg_am a ON a.oid = o.opcmethod WHERE o.opcname = $1::name "
				"AND o.opcnamespace = 'pg_catalog'::regnamespace AND quote_ident(a.amname) = $2";
		else if (objs[i].kind == WS_OBJ_OPFAMILY)
			sql = "SELECT 1 FROM pg_opfamily o JOIN pg_am a ON a.oid = o.opfmethod WHERE o.opfname = $1::name "
				"AND o.opfnamespace = 'pg_catalog'::regnamespace AND quote_ident(a.amname) = $2";
		else
			continue;
		here = name_here(&objs[i], objs[i].name);
		method = objs[i].args != NULL ? ws_text_from_utf8(objs[i].args) : "";
		params[0] = here;
		params[1] = method;
		WsSql->read(sql, objs[i].kind == WS_OBJ_COLLATION ? 1 : 2, params, &rows);
		if (rows.nrows > 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s \"%s\" of schema \"%s\" is named as one in this server's catalogs, which what uses it would be taken to mean",
							objs[i].kind == WS_OBJ_COLLATION ? "collation" :
							objs[i].kind == WS_OBJ_OPCLASS ? "operator class" : "operator family",
							here, schema)));
		ws_sql_rows_free(&rows);
		pfree(here);
	}
}

void
ws_obj_restore(const WsObject *objs, int n, const char *schema, bool before_tables)
{
	int			level;
	int			nest;
	int		   *order = palloc_array(int, Max(n, 1));
	bool		any = false;

	ws_obj_restore_order(objs, n, order);
	for (int i = 0; i < n; i++)
		any |= ws_obj_before_tables(&objs[i]) == before_tables || (!before_tables && objs[i].owned_table != NULL);
	if (!any)
	{
		pfree(order);
		return;
	}
	check_collations_are_free(objs, n, schema);
	for (int i = 0; i < n; i++)
		if (ws_obj_before_tables(&objs[i]) == before_tables && !is_of_table(objs[i].kind) &&
			objs[i].kind != WS_OBJ_CAST && objs[i].kind != WS_OBJ_COLLATION && objs[i].kind != WS_OBJ_CONVERSION &&
			!is_of_operators(objs[i].kind))
			ws_schema_check_defaults(schema, is_function(objs[i].kind) ? DEFACLOBJ_FUNCTION :
									 is_relation(objs[i].kind) ? DEFACLOBJ_RELATION :
									 objs[i].kind == WS_OBJ_SEQUENCE ? DEFACLOBJ_SEQUENCE : DEFACLOBJ_TYPE);
	level = WsSql->begin();
	check_nothing_captures(schema);
	nest = ws_schema_path_begin(schema);
	/* (a body in text names what may not be made yet, as a function needing another may) */
	(void) set_config_option("check_function_bodies", "off", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	for (int i = 0; i < n; i++)
	{
		Buf			sql = BUF_INIT;

		if (ws_obj_before_tables(&objs[order[i]]) != before_tables)
			continue;
		ws_obj_create_statement(&objs[order[i]], schema, &sql);
		run_statement(&sql);
	}
	/*
	 * Comments first, all of them, as who restores may not own them once owners are
	 * set: a relation's owner is its index's too.  Under the path still, as a function's
	 * statements say its input types bare.
	 */
	for (int i = 0; i < n; i++)
	{
		const WsObject *o = &objs[order[i]];

		if (ws_obj_before_tables(o) != before_tables)
			continue;
		for (int k = 0; k < o->ncomments; k++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_comment_statement(o, k, schema, &sql);
			run_statement(&sql);
		}
	}
	for (int i = 0; i < n; i++)
	{
		const WsObject *o = &objs[order[i]];
		Buf			owner = BUF_INIT;

		if (ws_obj_before_tables(o) != before_tables)
			continue;
		ws_obj_owner_statement(o, schema, &owner);
		if (owner.len > 0)
			run_statement(&owner);
		buf_free(&owner);
		{
			Buf			fires = BUF_INIT;

			fires_statement(o, schema, &fires);
			if (fires.len > 0)
				run_statement(&fires);
			buf_free(&fires);
		}

		/*
		 * With SET and not INHERIT on its owner, one may give it away and then a grant
		 * or a revoke of it only warns, leaving it open.  Not with us.
		 */
		if ((o->ngrants > 0 || o->public_revoked) && o->owner != NULL)
		{
			char	   *owner_here = ws_name_from_utf8(o->owner);

			if (!has_privs_of_role(GetUserId(), get_role_oid(owner_here, false)))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("cannot restore the grants of \"%s\": this role does not have the privileges of its owner \"%s\"",
								o->name, owner_here)));
			pfree(owner_here);
		}
		if (o->public_revoked)
		{
			Buf			sql = BUF_INIT;

			ws_put_text(&sql, "REVOKE ALL ON ROUTINE ");
			put_object(o, schema, &sql);
			ws_put_text(&sql, " FROM PUBLIC");
			run_statement(&sql);
		}
		/* (after its owner: as owner, a grant to who restores it would be no grant) */
		for (int k = 0; k < o->ngrants; k++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_grant_statement(o, k, schema, &sql);
			run_statement(&sql);
		}
	}
	if (!before_tables)
		for (int i = 0; i < n; i++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_owned_statement(&objs[i], schema, &sql);
			if (sql.len > 0)
				run_statement(&sql);
			buf_free(&sql);
		}
	ws_schema_path_end(nest);
	WsSql->end(level);
	pfree(order);
}
