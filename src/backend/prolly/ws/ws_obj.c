/*-------------------------------------------------------------------------
 *
 * ws_obj.c
 *	  A versioned schema's objects other than tables (prolly/ws/ws_obj.h), and
 *	  "binary", their codec.
 *
 * Its key: kind's word, a slash, name.  Its value, each string a uvarint length and
 * bytes: definition; uvarint ndeps and keys; owner; uvarint ncomments
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
	[WS_OBJ_MATVIEW] = "matview",
	[WS_OBJ_VIEW] = "view",
};

static const char *const kind_sql[] = {
	[WS_OBJ_MATVIEW] = "MATERIALIZED VIEW",
	[WS_OBJ_VIEW] = "VIEW",
};

static bool
privileges_problem(const char *p)
{
	if (p == NULL || p[0] == '\0')
		return true;
	for (; *p; p++)
	{
		if (strchr("adDmrtwx", *p) == NULL)
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
	if (bad_name(o->owner) || IsReservedName(o->owner))
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
	o->owner = get_string(&p, end, false);
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
"  WHEN EXISTS (SELECT 1 FROM pg_seclabel l WHERE l.classoid = 'pg_class'::regclass AND l.objoid = c.oid) "
"    THEN 'it has a security label' "
"  WHEN c.relacl IS NOT NULL AND (SELECT count(*) FROM aclexplode(c.relacl) e WHERE e.grantee = c.relowner) <> "
"      (SELECT count(*) FROM aclexplode(acldefault('r', c.relowner))) THEN 'its owner''s rights have been changed' "
"  WHEN EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"        JOIN pg_type t ON (d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid AND t.typrelid = c.oid) "
"      WHERE r.ev_class = c.oid AND d.deptype = 'n') "
"    OR EXISTS (SELECT 1 FROM pg_rewrite r JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
"      WHERE r.ev_class = c.oid AND d.deptype = 'n' AND d.refclassid = 'pg_class'::regclass AND d.refobjid = c.oid "
"        AND d.refobjsubid <> 0) "
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
"        OR (d.refclassid = 'pg_type'::regclass AND EXISTS (SELECT 1 FROM pg_type t JOIN pg_class q ON q.oid = t.typrelid "
"           WHERE t.oid = d.refobjid AND q.relkind IN ('r', 'p', 'v', 'm'))) "
"        OR coalesce((SELECT i.schema = 'pg_catalog' FROM pg_identify_object(d.refclassid, d.refobjid, 0) i), false))) "
"    THEN 'it needs an object no commit holds yet' "
"  END "
"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
"WHERE n.nspname = $1::name AND c.relkind IN ('v', 'm') ORDER BY 1";

/*
 * What prints a definition reads its names by the catalog as it is now, whatever
 * snapshot is active.  So read as it is now too: lock each object and what it needs,
 * which keeps them from being renamed, until a new snapshot names nothing more to
 * lock.  That snapshot stays pushed, for the caller to pop.
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
}

/* Whether a printed query must say what it can only say by naming its schema. */
typedef struct NamesSchema
{
	const char *schema;
	const char *why;
} NamesSchema;

static bool
qualified_by(List *names, const char *schema)
{
	return list_length(names) >= 2 && IsA(linitial(names), String) &&
		strcmp(strVal(linitial(names)), schema) == 0;
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
				strcmp(((RangeVar *) node)->schemaname, n->schema) == 0;
			break;
		case T_TypeName:
			named = qualified_by(((TypeName *) node)->names, n->schema);
			break;
		case T_FuncCall:
			named = qualified_by(((FuncCall *) node)->funcname, n->schema);
			break;
		case T_A_Expr:
			named = qualified_by(((A_Expr *) node)->name, n->schema);
			break;
		case T_CollateClause:
			named = qualified_by(((CollateClause *) node)->collname, n->schema);
			break;
		case T_SortBy:
			named = qualified_by(((SortBy *) node)->useOp, n->schema);
			break;
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
		n->why = "it must say its schema's name, having something named as the catalogs' is";
		return true;
	}
	return raw_expression_tree_walker(node, names_schema_walker, arg);
}

/* Refuses a printed query that would not mean the same in another schema. */
static void
check_query_is_portable(const char *name, const char *schema, const char *query)
{
	List	   *parsed = raw_parser(query, RAW_PARSE_DEFAULT);
	NamesSchema n = {schema, NULL};

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

static void
read_deps(WsObject *o, const char *oid)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT DISTINCT q.relkind::text, q.relname::text FROM pg_rewrite r "
				"JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid "
				"LEFT JOIN pg_type t ON d.refclassid = 'pg_type'::regclass AND t.oid = d.refobjid "
				"JOIN pg_class q ON q.oid = CASE WHEN d.refclassid = 'pg_class'::regclass THEN d.refobjid ELSE t.typrelid END "
				"WHERE r.ev_class = $1::oid AND d.deptype = 'n' AND q.oid <> $1::oid AND q.relkind IN ('v', 'm')",
				1, params, &rows);
	o->deps = zeroed(rows.nrows, sizeof(char *));
	o->ndeps = rows.nrows;
	for (int i = 0; i < rows.nrows; i++)
	{
		Buf			key = BUF_INIT;
		char	   *name = ws_name_to_utf8(ws_sql_value(&rows, i, 1));

		ws_obj_key(ws_sql_value(&rows, i, 0)[0] == 'm' ? WS_OBJ_MATVIEW : WS_OBJ_VIEW, name, &key);
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

void
ws_obj_read(const char *schema, WsObject **out, int *n)
{
	int			level = WsSql->begin();
	int			nest;
	const char *params[1] = {schema};
	WsSqlRows	rels;
	WsObject   *objs;

	push_locked_snapshot(schema);
	refuse_what_cannot_be_held(schema);
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
		read_deps(o, oid);
		read_comments(o, oid);
		read_owner_and_grants(o, oid, name);
	}
	if (rels.nrows > 1)
		qsort(objs, rels.nrows, sizeof(WsObject), object_cmp);
	{
		/* (views can be made to need each other, which no restore could make) */
		int		   *order = palloc_array(int, Max(rels.nrows, 1));

		if (!compute_order(objs, rels.nrows, order))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("schema \"%s\" cannot be held in a commit: its views need each other in a cycle", schema)));
		pfree(order);
	}
	*n = rels.nrows;
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
	ws_put_text(out, " ON TABLE ");
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
 * Refuses a schema with anything a definition's unqualified name could take for what
 * it meant: an exact match here is preferred to a polymorphic one in the catalogs,
 * though the catalogs come first.  A commit's tables are here, and name nothing.
 */
static void
check_nothing_captures(const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->run("SELECT what FROM (SELECT 'a function' AS what FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
			   "    WHERE n.nspname = $1::name "
			   "  UNION ALL SELECT 'an operator' FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace "
			   "    WHERE n.nspname = $1::name "
			   "  UNION ALL SELECT 'a collation' FROM pg_collation c JOIN pg_namespace n ON n.oid = c.collnamespace "
			   "    WHERE n.nspname = $1::name "
			   "  UNION ALL SELECT 'a type' FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
			   "    WHERE n.nspname = $1::name AND t.typrelid = 0 "
			   "      AND NOT EXISTS (SELECT 1 FROM pg_type a WHERE a.typarray = t.oid)) w LIMIT 1", 1, params, &rows);
	if (rows.nrows > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("schema \"%s\" holds %s no commit holds, which the commit's definitions could be taken for",
						schema, ws_sql_value(&rows, 0, 0))));
	ws_sql_rows_free(&rows);
}

void
ws_obj_restore(const WsObject *objs, int n, const char *schema)
{
	int			level;
	int			nest;
	int		   *order = palloc_array(int, Max(n, 1));

	ws_obj_restore_order(objs, n, order);
	if (n == 0)
	{
		pfree(order);
		return;
	}
	ws_schema_check_relation_defaults(schema);
	level = WsSql->begin();
	check_nothing_captures(schema);
	nest = path_begin(schema);
	for (int i = 0; i < n; i++)
	{
		Buf			sql = BUF_INIT;

		ws_obj_create_statement(&objs[order[i]], schema, &sql);
		run_statement(&sql);
	}
	path_end(nest);
	for (int i = 0; i < n; i++)
	{
		const WsObject *o = &objs[order[i]];

		for (int k = 0; k < o->ncomments; k++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_comment_statement(o, k, schema, &sql);
			run_statement(&sql);
		}
		for (int k = 0; k < o->ngrants; k++)
		{
			Buf			sql = BUF_INIT;

			ws_obj_grant_statement(o, k, schema, &sql);
			run_statement(&sql);
		}
		{
			Buf			sql = BUF_INIT;

			ws_obj_owner_statement(o, schema, &sql);
			run_statement(&sql);
		}
	}
	WsSql->end(level);
	pfree(order);
}
