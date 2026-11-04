/*-------------------------------------------------------------------------
 *
 * ws_obj.c
 *	  A versioned schema's objects other than tables (prolly/ws/ws_obj.h), and
 *	  "binary", their codec.
 *
 * Its key: kind's word, a slash, name.  Its value, each string a uvarint length and
 * bytes: definition; uvarint ndeps and keys; owner, empty for none; uvarint ncomments
 * and each one's column, empty for none, and text; uvarint ngrants and each one's
 * column, empty for none, grantee, empty for PUBLIC, grant option byte, privileges.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_obj.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_namespace.h"
#include "common/int.h"
#include "lib/binaryheap.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "prolly/ws/ws_obj.h"
#include "storage/lmgr.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_tags.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/snapmgr.h"

const WsObjectCodecRoutine *WsObjectCodec = &BinaryWsObjectCodec;

static const char *const kind_words[] = {
	[WS_OBJ_DOMAIN] = "domain",
	[WS_OBJ_MATVIEW] = "matview",
	[WS_OBJ_TYPE] = "type",
	[WS_OBJ_VIEW] = "view",
};

static const char *const kind_sql[] = {
	[WS_OBJ_DOMAIN] = "DOMAIN",
	[WS_OBJ_MATVIEW] = "MATERIALIZED VIEW",
	[WS_OBJ_TYPE] = "TYPE",
	[WS_OBJ_VIEW] = "VIEW",
};

bool
ws_obj_before_tables(WsObjectKind kind)
{
	return kind == WS_OBJ_DOMAIN || kind == WS_OBJ_TYPE;
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
		if (strchr("adDmrtUwx", *p) == NULL)
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

		if (strncmp(key, kind_words[i], n) == 0 && key[n] == '/' && !bad_name(key + n + 1))
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
	if (bad_name(o->name))
		return "an object of no name, or one too long";
	if (o->definition == NULL || !ws_tags_well_formed(o->definition))
		return "a definition that is not stored text";
	if (o->owner == NULL ? is_relation(o->kind) : bad_name(o->owner) || IsReservedName(o->owner))
		return "no owner, or one no commit holds";
	ws_obj_key(o->kind, o->name, &self);
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
ws_obj_key(WsObjectKind kind, const char *name, Buf *out)
{
	ws_put_text(out, kind_words[kind]);
	ws_put_text(out, "/");
	ws_put_text(out, name);
}

static void
obj_encode_key(const WsObject *o, Buf *out)
{
	const char *problem = ws_obj_problem(o);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	ws_obj_key(o->kind, o->name, out);
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

		ws_obj_key(objs[mid].kind, objs[mid].name, &k);
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

/*
 * The search path a definition is written and run under.  The catalogs come first
 * and temporary objects last, so that nothing of the schema's can be taken for
 * theirs or the other way about: a name the schema shadows is written qualified.
 */
static int
path_begin(const char *schema)
{
	int			nest = NewGUCNestLevel();
	char	   *path = psprintf("pg_catalog, %s, pg_temp", quote_identifier(schema));

	(void) set_config_option("search_path", path, PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	pfree(path);
	return nest;
}

static void
path_end(int nest)
{
	AtEOXact_GUC(true, nest);
}

/* What no commit can hold of an object in the schema, the first of it. */
static const char *const refusal_sql =
"SELECT c.relname::text, CASE "
"  WHEN EXISTS (SELECT 1 FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid "
"      AND d.deptype = 'e') THEN 'it belongs to an extension' "
"  WHEN c.reltablespace <> 0 THEN 'it has a tablespace of its own' "
"  WHEN c.relkind = 'm' AND c.relam <> (SELECT oid FROM pg_am WHERE amname = 'heap') "
"    THEN 'it has another access method' "
"  WHEN EXISTS (SELECT 1 FROM pg_index i WHERE i.indrelid = c.oid) THEN 'it has an index' "
"  WHEN EXISTS (SELECT 1 FROM pg_trigger t WHERE t.tgrelid = c.oid AND NOT t.tgisinternal) THEN 'it has a trigger' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r WHERE r.ev_class = c.oid AND r.rulename <> '_RETURN') THEN 'it has a rule' "
"  WHEN EXISTS (SELECT 1 FROM pg_attrdef a WHERE a.adrelid = c.oid) THEN 'it has a default on a column' "
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
"      WHERE r.ev_class = c.oid) THEN 'it has a comment on its rule' "
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = 'pg_class'::regclass AND l.objoid = c.oid) "
"    THEN 'it has a security label' "
"  WHEN c.relacl IS NOT NULL AND (SELECT count(*) FROM aclexplode(c.relacl) e WHERE e.grantee = c.relowner) <> "
"      (SELECT count(*) FROM aclexplode(acldefault('r', c.relowner))) THEN 'its owner''s rights have been changed' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"        JOIN pg_type t ON (d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid AND t.typrelid = c.oid) "
"      WHERE r.ev_class = c.oid AND d.deptype = 'n') "
"    OR EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"      WHERE r.ev_class = c.oid AND d.deptype = 'n' AND d.refclassid = 'pg_class'::regclass AND d.refobjid = c.oid) "
"    THEN 'it needs itself, as no commit could make it' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"        CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i "
"      WHERE r.ev_class = c.oid AND d.deptype = 'n' AND i.schema IS NOT NULL "
"        AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) "
"    THEN 'it needs something in another schema' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"      WHERE r.ev_class = c.oid AND d.deptype = 'n' AND NOT ( "
"        (d.refclassid = 'pg_class'::regclass AND EXISTS (SELECT 1 FROM pg_class q WHERE q.oid = d.refobjid "
"           AND (q.relkind IN ('r', 'p', 'v', 'm') OR q.relnamespace = 'pg_catalog'::regnamespace))) "
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

	WsSql->read("SELECT coalesce(encode(sha256(convert_to(string_agg(x, ',' ORDER BY x), 'UTF8')), 'hex'), '') FROM ( "
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
				"  UNION ALL SELECT 't' || t.oid || ':' || t.xmin FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'p' || t.oid || ':' || r.rolname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    JOIN pg_roles r ON r.oid = t.typowner WHERE n.nspname = $1::name "
				"  UNION ALL SELECT 'q' || t.oid || ':' || r.rolname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
				"    CROSS JOIN LATERAL aclexplode(t.typacl) e JOIN pg_roles r ON r.oid = e.grantee WHERE n.nspname = $1::name "
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
 * schema's relations differ from it, to try again.
 */
static void
push_locked_snapshot(const char *schema)
{
	const char *params[1] = {schema};
	HTAB	   *locked;
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
	if (IsolationUsesXactSnapshot())
	{
		char	   *now = relations_version(schema);
		char	   *then;

		PushCopiedSnapshot(GetTransactionSnapshot());
		UpdateActiveSnapshotCommandId();
		then = relations_version(schema);
		PopActiveSnapshot();
		if (strcmp(now, then) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not read schema \"%s\": it has changed since this transaction's snapshot", schema)));
		pfree(now);
		pfree(then);
	}
}

/*
 * What a printed query says by a schema's name it says of that schema, which is no
 * name another commit's restore would have: only the catalogs may be named.
 */
typedef struct NamesSchema
{
	const char *why;
} NamesSchema;

static bool
qualified(List *names)
{
	return list_length(names) >= 2 && IsA(linitial(names), String) &&
		strcmp(strVal(linitial(names)), "pg_catalog") != 0;
}

/* (a reg* constant is a name in a string, which only the type it is cast to says) */
static bool
string_is_qualified(const char *s)
{
	const char *p = s;
	bool		quoted = false;
	const char *start;

	while (*p == ' ')
		p++;
	start = p;
	for (; *p; p++)
	{
		if (*p == '"')
			quoted = !quoted;
		else if (!quoted && *p == '(')
			return false;
		else if (!quoted && *p == '.')
			return !((size_t) (p - start) == strlen("pg_catalog") &&
					 strncmp(start, "pg_catalog", p - start) == 0);
	}
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
				strcmp(((RangeVar *) node)->schemaname, "pg_catalog") != 0;
			break;
		case T_TypeName:
			named = qualified(((TypeName *) node)->names);
			break;
		case T_FuncCall:
			named = qualified(((FuncCall *) node)->funcname);
			break;
		case T_A_Expr:
			named = qualified(((A_Expr *) node)->name);
			break;
		case T_CollateClause:
			named = qualified(((CollateClause *) node)->collname);
			break;
		case T_SortBy:
			named = qualified(((SortBy *) node)->useOp);
			break;
		case T_TypeCast:
			{
				TypeCast   *c = (TypeCast *) node;
				const char *type = strVal(llast(c->typeName->names));

				if (strncmp(type, "reg", 3) == 0 && c->typeName->arrayBounds != NIL)
				{
					/* (no dependency is held for a name in one, nor is it one name) */
					n->why = "it has an array of reg* names, which say nothing of what they need";
					return true;
				}
				if (strncmp(type, "reg", 3) == 0 && IsA(c->arg, A_Const) &&
					IsA(&((A_Const *) c->arg)->val, String) &&
					string_is_qualified(strVal(&((A_Const *) c->arg)->val)))
					named = true;
				break;
			}
		case T_RangeSubselect:
			{
				RangeSubselect *r = (RangeSubselect *) node;

				/*
				 * (VALUES with column names prints as a subquery that prints
				 * otherwise once it is one: two sets of bytes for one view)
				 */
				if (r->alias != NULL && strcmp(r->alias->aliasname, "*VALUES*") == 0)
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

/* Refuses a printed query that would not mean the same in another schema. */
static void
check_query_is_portable(const char *name, const char *schema, const char *query)
{
	List	   *parsed = raw_parser(query, RAW_PARSE_DEFAULT);
	NamesSchema n = {NULL};

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

/*
 * What an object needs among a schema's objects: of what depends for it (source, its
 * classid and objid, of $1), the views and the types.  An array is its element's and
 * a multirange its range's, which are what is made.  A table is no need: tables are
 * all made, or a type that needs one is refused.
 */
static void
read_deps(WsObject *o, const char *oid, const char *source)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;
	char	   *sql;

	sql = psprintf("WITH src(classid, objid) AS (%s), "
				   "d AS (SELECT p.refclassid, p.refobjid FROM pg_depend p JOIN src ON p.classid = src.classid "
				   "  AND p.objid = src.objid WHERE p.deptype = 'n') "
				   "SELECT DISTINCT q.relkind::text, q.relname::text FROM d "
				   "LEFT JOIN pg_type t ON d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid "
				   "JOIN pg_class q ON q.oid = CASE WHEN d.refclassid = 'pg_class'::regclass THEN d.refobjid ELSE t.typrelid END "
				   "WHERE q.relkind IN ('v', 'm') AND q.relnamespace = (SELECT nsp FROM (SELECT relnamespace AS nsp FROM pg_class WHERE oid = $1::oid "
				   "    UNION ALL SELECT typnamespace FROM pg_type WHERE oid = $1::oid) n LIMIT 1) "
				   "UNION SELECT CASE b.typtype WHEN 'd' THEN 'D' ELSE 'T' END, b.typname::text FROM d "
				   "JOIN pg_type t ON d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid "
				   "LEFT JOIN pg_type e ON e.oid = t.typelem AND e.typarray = t.oid "
				   "LEFT JOIN pg_range r ON r.rngmultitypid = coalesce(e.oid, t.oid) "
				   "JOIN pg_type b ON b.oid = coalesce(r.rngtypid, e.oid, t.oid) "
				   "LEFT JOIN pg_class c ON c.oid = b.typrelid "
				   "WHERE b.oid <> $1::oid AND (c.oid IS NULL OR c.relkind = 'c') "
				   "  AND b.typnamespace NOT IN ('pg_catalog'::regnamespace)", source);
	WsSql->read(sql, 1, params, &rows);
	pfree(sql);
	o->deps = zeroed(rows.nrows, sizeof(char *));
	o->ndeps = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		Buf			key = BUF_INIT;
		char	   *name = ws_name_to_utf8(ws_sql_value(&rows, i, 1));
		char		k = ws_sql_value(&rows, i, 0)[0];

		ws_obj_key(k == 'm' ? WS_OBJ_MATVIEW : k == 'v' ? WS_OBJ_VIEW : k == 'D' ? WS_OBJ_DOMAIN : WS_OBJ_TYPE,
				   name, &key);
		buf_append_byte(&key, 0);
		o->deps[i] = px_memdup(key.data, key.len);
		buf_free(&key);
	}
	if (o->ndeps > 1)
		qsort(o->deps, o->ndeps, sizeof(char *), cstr_cmp);
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
read_owner_and_grants(WsObject *o, const char *oid, const char *name)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT r.rolname::text, r.rolcanlogin::text, "
				"  (r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)::text "
				"FROM pg_class c JOIN pg_roles r ON r.oid = c.relowner WHERE c.oid = $1::oid", 1, params, &rows);
	/* (it reads with its owner's rights, so another owner could show other rows) */
	if (rows.nrows != 1 || ws_sql_value(&rows, 0, 1)[0] == 't' || ws_sql_value(&rows, 0, 2)[0] == 't' ||
		IsReservedName(ws_sql_value(&rows, 0, 0)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" cannot be held in a commit: it is owned by a role that cannot travel", name),
				 errhint("Make a role that cannot log in and holds no powers its owner, with ALTER ... OWNER TO.")));
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
				"    WHEN 'TRIGGER' THEN 't' WHEN 'MAINTAIN' THEN 'm' END, '') "
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
	int			c = strcmp(kind_words[x->kind], kind_words[y->kind]);

	return c != 0 ? c : strcmp(x->name, y->name);
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
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = 'pg_type'::regclass AND l.objoid = t.oid) "
"    THEN 'it has a security label' "
"  WHEN t.typacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(t.typacl) e WHERE e.grantee = 0) "
"    THEN 'its USAGE has been revoked from PUBLIC' "
"  WHEN t.typacl IS NOT NULL AND NOT EXISTS (SELECT 1 FROM aclexplode(t.typacl) e WHERE e.grantee = t.typowner) "
"    THEN 'its owner''s rights have been changed' "
"  WHEN EXISTS (SELECT 1 FROM pg_range r JOIN pg_type m ON m.oid = r.rngmultitypid "
"      WHERE r.rngtypid = t.oid AND (m.typacl IS NOT NULL "
"        OR m.typname <> left(CASE WHEN strpos(t.typname, 'range') > 0 THEN regexp_replace(t.typname, 'range', 'multirange') "
"            ELSE t.typname || '_multirange' END, 63) "
"        OR EXISTS (SELECT 1 FROM pg_description x WHERE x.classoid = 'pg_type'::regclass AND x.objoid = m.oid))) "
"    THEN 'its multirange type has a name, comment or grant of its own' "
"  WHEN EXISTS (SELECT 1 FROM pg_attribute a WHERE c.oid IS NOT NULL AND a.attrelid = c.oid AND a.attnum > 0 "
"      AND NOT a.attisdropped AND a.attstattarget >= 0) THEN 'it has settings on a column' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"        CROSS JOIN LATERAL pg_identify_object(d.refclassid, d.refobjid, 0) i "
"      WHERE d.deptype = 'n' AND i.schema IS NOT NULL AND i.schema <> 'pg_catalog' AND i.schema <> quote_ident(n.nspname)) "
"    THEN 'it needs something in another schema' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"      WHERE d.deptype = 'n' AND d.refclassid = 'pg_type'::regclass AND EXISTS (SELECT 1 FROM pg_type y JOIN pg_class q ON q.oid = y.typrelid "
"        WHERE y.oid = d.refobjid AND q.relkind <> 'c')) "
"    THEN 'it needs a table''s row type, and a table may need it' "
"  WHEN EXISTS (SELECT 1 FROM (" TYPE_SOURCE_OF("t.oid") ") s(classid, objid) JOIN pg_depend d ON d.classid = s.classid AND d.objid = s.objid "
"      WHERE d.deptype = 'n' AND d.refclassid <> 'pg_type'::regclass "
"        AND NOT (d.refclassid = 'pg_namespace'::regclass AND d.refobjid = n.oid AND s.classid <> 'pg_constraint'::regclass) "
"        AND NOT coalesce((SELECT i.schema = 'pg_catalog' "
"        FROM pg_identify_object(d.refclassid, d.refobjid, 0) i), false)) "
"    THEN 'it needs an object no commit holds yet' "
"  END " TYPES_OF_SCHEMA "ORDER BY 1";

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
	NamesSchema n = {NULL};
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
			else if (IsA(arg, List) && qualified((List *) arg))
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

/* The statement after its name of a type of each kind, into text. */
static void
type_definition(char typtype, const char *oid, Buf *text)
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
			WsSql->read("SELECT coalesce(string_agg(quote_ident(a.attname) || ' ' || format_type(a.atttypid, a.atttypmod) || "
						"  CASE WHEN a.attcollation <> u.typcollation "
						"    THEN ' COLLATE ' || quote_ident(k.collname) ELSE '' END, ', ' ORDER BY a.attnum), '') "
						"FROM pg_type t JOIN pg_attribute a ON a.attrelid = t.typrelid "
						"JOIN pg_type u ON u.oid = a.atttypid LEFT JOIN pg_collation k ON k.oid = a.attcollation "
						"WHERE t.oid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped", 1, params, &rows);
			ws_put_text(text, "AS (");
			ws_put_text(text, ws_sql_value(&rows, 0, 0));
			ws_put_text(text, ")");
			break;
		case 'r':
			WsSql->read("SELECT 'AS RANGE (SUBTYPE = ' || format_type(r.rngsubtype, NULL) || "
						"  CASE WHEN o.opcdefault THEN '' ELSE ', SUBTYPE_OPCLASS = ' || quote_ident(o.opcname) END || "
						"  CASE WHEN r.rngcollation <> s.typcollation "
						"    THEN ', COLLATION = ' || quote_ident(k.collname) ELSE '' END || "
						"  CASE WHEN r.rngcanonical <> 0 THEN ', CANONICAL = ' || r.rngcanonical::regproc ELSE '' END || "
						"  CASE WHEN r.rngsubdiff <> 0 THEN ', SUBTYPE_DIFF = ' || r.rngsubdiff::regproc ELSE '' END || ')' "
						"FROM pg_range r JOIN pg_type s ON s.oid = r.rngsubtype JOIN pg_opclass o ON o.oid = r.rngsubopc "
						"LEFT JOIN pg_collation k ON k.oid = r.rngcollation WHERE r.rngtypid = $1::oid", 1, params, &rows);
			ws_put_text(text, ws_sql_value(&rows, 0, 0));
			break;
		case 'd':
			WsSql->read("SELECT 'AS ' || format_type(t.typbasetype, t.typtypmod) || "
						"  CASE WHEN t.typcollation <> b.typcollation "
						"    THEN ' COLLATE ' || quote_ident(k.collname) ELSE '' END || "
						"  coalesce(' DEFAULT ' || pg_get_expr(t.typdefaultbin, 0), '') || "
						"  coalesce((SELECT string_agg(' CONSTRAINT ' || quote_ident(c.conname) || ' ' || "
						"      pg_get_constraintdef(c.oid), '' ORDER BY c.conname) "
						"    FROM pg_constraint c WHERE c.contypid = t.oid), '') "
						"FROM pg_type t JOIN pg_type b ON b.oid = t.typbasetype "
						"LEFT JOIN pg_collation k ON k.oid = t.typcollation WHERE t.oid = $1::oid", 1, params, &rows);
			ws_put_text(text, ws_sql_value(&rows, 0, 0));
			break;
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
		type_definition(typtype, oid, &text);
		buf_append_byte(&text, 0);
		check_type_is_portable(name, schema, typtype == 'd' ? "CREATE DOMAIN" : "CREATE TYPE", (char *) text.data);
		ws_tags_escape(ws_text_to_utf8((char *) text.data), &stored);
		buf_append_byte(&stored, 0);
		o->definition = px_memdup(stored.data, stored.len);
		buf_free(&stored);
		buf_free(&text);
		read_deps(o, oid, TYPE_SOURCE_OF("$1::oid"));
		read_type_comments(o, oid);
		read_type_owner_and_grants(o, oid, name);
	}
	ws_sql_rows_free(&types);
	return objs;
}

/* (without it, what is in the schema could only be printed with its schema's name) */
static bool
has_usage_of_schema(const char *schema)
{
	Oid			nsp = get_namespace_oid(schema, true);

	return !OidIsValid(nsp) ||
		object_aclcheck(NamespaceRelationId, nsp, GetUserId(), ACL_USAGE) == ACLCHECK_OK;
}

void
ws_obj_read(const char *schema, WsObject **out, int *n)
{
	int			level = WsSql->begin();
	int			nest;
	const char *params[1] = {schema};
	WsSqlRows	rels;
	WsObject   *objs;
	int			total;

	if (!has_usage_of_schema(schema))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for schema %s", schema)));
	push_locked_snapshot(schema);
	refuse_what_cannot_be_held(schema);
	refuse_types(schema, type_refusal_sql);
	nest = path_begin(schema);

	WsSql->read("SELECT c.oid::text, c.relkind::text, c.relname::text, pg_get_viewdef(c.oid, false), "
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
		const char *viewdef = ws_sql_value(&rels, i, 3);
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
		read_deps(o, oid, "SELECT 'pg_rewrite'::regclass::oid, oid FROM pg_rewrite WHERE ev_class = $1::oid");
		read_comments(o, oid);
		read_owner_and_grants(o, oid, name);
	}
	total = rels.nrows;
	objs = read_types(schema, objs, &total);
	if (total > 1)
		qsort(objs, total, sizeof(WsObject), object_cmp);
	{
		/* (views can be made to need each other, which no restore could make) */
		int		   *order = palloc_array(int, Max(total, 1));

		if (!compute_order(objs, total, order))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("schema \"%s\" cannot be held in a commit: its views need each other in a cycle", schema)));
		pfree(order);
	}
	*n = total;
	*out = objs;
	ws_sql_rows_free(&rels);
	path_end(nest);
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
	here = ws_name_from_utf8(o->name);
	definition = ws_text_from_utf8(o->definition);
	ws_put_text(&sql, "CREATE ");
	ws_put_text(&sql, kind_sql[o->kind]);
	ws_put_text(&sql, " ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, here);
	ws_put_text(&sql, " ");
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
	char	   *here = ws_name_from_utf8(o->name);

	ws_put_identifier(sql, schema);
	ws_put_text(sql, ".");
	ws_put_identifier(sql, here);
	pfree(here);
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
	ws_put_text(out, is_relation(o->kind) ? "TABLE" : kind_sql[o->kind]);
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

void
ws_obj_restore(const WsObject *objs, int n, const char *schema, bool before_tables)
{
	int			level;
	int			nest;
	int		   *order = palloc_array(int, Max(n, 1));
	bool		any = false;

	ws_obj_restore_order(objs, n, order);
	for (int i = 0; i < n; i++)
		any |= ws_obj_before_tables(objs[i].kind) == before_tables;
	if (!any)
	{
		pfree(order);
		return;
	}
	for (int i = 0; i < n; i++)
		if (ws_obj_before_tables(objs[i].kind) == before_tables)
			ws_schema_check_defaults(schema, before_tables ? DEFACLOBJ_TYPE : DEFACLOBJ_RELATION);
	level = WsSql->begin();
	check_nothing_captures(schema);
	nest = path_begin(schema);
	for (int i = 0; i < n; i++)
	{
		Buf			sql = BUF_INIT;

		if (ws_obj_before_tables(objs[order[i]].kind) != before_tables)
			continue;
		ws_obj_create_statement(&objs[order[i]], schema, &sql);
		run_statement(&sql);
	}
	path_end(nest);
	for (int i = 0; i < n; i++)
	{
		const WsObject *o = &objs[order[i]];
		Buf			owner = BUF_INIT;

		if (ws_obj_before_tables(o->kind) != before_tables)
			continue;
		for (int k = 0; k < o->ncomments; k++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_comment_statement(o, k, schema, &sql);
			run_statement(&sql);
		}
		ws_obj_owner_statement(o, schema, &owner);
		if (owner.len > 0)
			run_statement(&owner);
		buf_free(&owner);
		/* (after its owner: as owner, a grant to who restores it would be no grant) */
		for (int k = 0; k < o->ngrants; k++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_grant_statement(o, k, schema, &sql);
			run_statement(&sql);
		}
	}
	WsSql->end(level);
	pfree(order);
}
