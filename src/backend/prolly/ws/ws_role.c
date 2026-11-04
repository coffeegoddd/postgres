/*-------------------------------------------------------------------------
 *
 * ws_role.c
 *	  The roles that travel with a versioned schema (prolly/ws/ws_role.h), and
 *	  "binary", their codec.
 *
 * Its key: name.  Its value: flags byte, uvarint nmember_of, and each one's name
 * (uvarint length and bytes) and options byte.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_role.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define ROLE_NO_INHERIT 0x01

#define MEMBER_ADMIN 0x01
#define MEMBER_INHERIT 0x02
#define MEMBER_SET 0x04

const WsRoleCodecRoutine *WsRoleCodec = &BinaryWsRoleCodec;

static const char *
name_problem(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return "a role of no name";
	if (strlen(name) >= NAMEDATALEN)
		return "a role's name too long";
	if (strcmp(name, "public") == 0 || strcmp(name, "none") == 0)
		return "a role of a name no role can have";
	if (IsReservedName(name))
		return "a predefined role";
	return NULL;
}

const char *
ws_role_problem(const WsRole *r)
{
	const char *problem = name_problem(r->name);

	if (problem != NULL)
		return problem;
	if (r->nmember_of < 0)
		return "a negative number of memberships";
	for (int i = 0; i < r->nmember_of; i++)
	{
		const char *of = r->member_of[i].role;

		problem = name_problem(of);
		if (problem != NULL)
			return problem;
		if (strcmp(of, r->name) == 0)
			return "a role a member of itself";
		/* (one order, so one set of bytes) */
		if (i > 0 && strcmp(r->member_of[i - 1].role, of) >= 0)
			return "memberships not in order";
	}
	return NULL;
}

static void
role_encode_key(const WsRole *r, Buf *out)
{
	const char *problem = ws_role_problem(r);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	buf_append(out, r->name, strlen(r->name));
}

static void
role_encode_value(const WsRole *r, Buf *out)
{
	const char *problem = ws_role_problem(r);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	buf_append_byte(out, r->no_inherit ? ROLE_NO_INHERIT : 0);
	buf_append_uvarint(out, (uint64) r->nmember_of);
	for (int i = 0; i < r->nmember_of; i++)
	{
		const WsRoleMembership *m = &r->member_of[i];
		size_t		n = strlen(m->role);

		buf_append_uvarint(out, n);
		buf_append(out, m->role, n);
		buf_append_byte(out, (m->admin ? MEMBER_ADMIN : 0) | (m->inherit ? MEMBER_INHERIT : 0) |
						(m->set ? MEMBER_SET : 0));
	}
}

pg_noreturn static void
malformed(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a malformed role: %s", what)));
}

static char *
utf8_string(const uint8 *p, size_t n)
{
	char	   *s;

	if (n > PG_INT32_MAX)
		malformed("a string too long to be one");
	if (memchr(p, '\0', n) != NULL)
		malformed("a string with a NUL in it");
	if (!pg_verify_mbstr(PG_UTF8, (const char *) p, (int) n, true))
		malformed("a string not UTF8");
	s = px_malloc(n + 1);
	memcpy(s, p, n);
	s[n] = '\0';
	return s;
}

static void
role_decode(const uint8 *key, size_t klen, const uint8 *val, size_t vlen, WsRole *r)
{
	const uint8 *p = val;
	const uint8 *end = val + vlen;
	uint64		n;
	uint8		flags;
	const char *problem;

	memset(r, 0, sizeof(*r));
	r->name = utf8_string(key, klen);
	if (p >= end)
		malformed("it ends early");
	flags = *p++;
	if (flags & ~ROLE_NO_INHERIT)
		malformed("flags not known");
	r->no_inherit = (flags & ROLE_NO_INHERIT) != 0;
	if (!ws_read_canonical_uvarint(&p, end, &n))
		malformed("a number not in its shortest form, or cut short");
	/* (each is at least two bytes: a length and its options) */
	if (n > (uint64) (end - p) / 2)
		malformed("more memberships than bytes");
	r->member_of = px_malloc((size_t) Max(n, 1) * sizeof(WsRoleMembership));
	memset(r->member_of, 0, (size_t) Max(n, 1) * sizeof(WsRoleMembership));
	r->nmember_of = (int) n;
	for (int i = 0; i < r->nmember_of; i++)
	{
		uint64		len;
		uint8		options;

		if (!ws_read_canonical_uvarint(&p, end, &len))
			malformed("a number not in its shortest form, or cut short");
		if (len > (uint64) (end - p))
			malformed("a string runs past its end");
		r->member_of[i].role = utf8_string(p, (size_t) len);
		p += len;
		if (p >= end)
			malformed("it ends early");
		options = *p++;
		if (options & ~(MEMBER_ADMIN | MEMBER_INHERIT | MEMBER_SET))
			malformed("a membership's options not known");
		r->member_of[i].admin = (options & MEMBER_ADMIN) != 0;
		r->member_of[i].inherit = (options & MEMBER_INHERIT) != 0;
		r->member_of[i].set = (options & MEMBER_SET) != 0;
	}
	if (p != end)
		malformed("it has bytes left over");
	problem = ws_role_problem(r);
	if (problem != NULL)
		malformed(problem);
}

const WsRoleCodecRoutine BinaryWsRoleCodec = {
	.name = "binary",
	.encode_key = role_encode_key,
	.encode_value = role_encode_value,
	.decode = role_decode,
};

void
ws_role_free(WsRole *r)
{
	px_free(r->name);
	for (int i = 0; i < r->nmember_of; i++)
		px_free(r->member_of[i].role);
	px_free(r->member_of);
	memset(r, 0, sizeof(*r));
}

const char *
ws_role_travel_problem(const char *role)
{
	HeapTuple	tup;
	const char *problem = NULL;

	if (IsReservedName(role))
		return "it is predefined";
	tup = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_authid r = (Form_pg_authid) GETSTRUCT(tup);

		if (r->rolcanlogin)
			problem = "it can log in";
		else if (r->rolsuper || r->rolcreaterole || r->rolcreatedb || r->rolreplication || r->rolbypassrls)
			problem = "it has a power no commit holds";
		ReleaseSysCache(tup);
	}
	return problem;
}

void
ws_role_check_name(const char *what, const char *role)
{
	bool		ascii = true;
	int			encoding = -1;
	bool		many = false;
	Relation	databases;
	TableScanDesc scan;
	HeapTuple	tup;

	for (const char *b = role; *b; b++)
		ascii &= !IS_HIGHBIT_SET(*b);
	if (ascii)
		return;
	databases = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(databases, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		int			here = ((Form_pg_database) GETSTRUCT(tup))->encoding;

		many |= encoding >= 0 && here != encoding;
		encoding = here;
	}
	table_endscan(scan);
	table_close(databases, AccessShareLock);
	if (many || !pg_verifymbstr(role, (int) strlen(role), true))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s names a role not in ASCII, in a cluster whose databases' encodings differ, or not in this one's",
						what)));
}

/* ---------------------------------------------------------- the catalogs */

static int
role_cmp(const void *a, const void *b)
{
	return strcmp(((const WsRole *) a)->name, ((const WsRole *) b)->name);
}

static int
membership_cmp(const void *a, const void *b)
{
	return strcmp(((const WsRoleMembership *) a)->role, ((const WsRoleMembership *) b)->role);
}

/* (the names to read, in this database's encoding, as they are found) */
static void
add_name(char ***names, int *n, int *cap, const char *name)
{
	for (int i = 0; i < *n; i++)
		if (strcmp((*names)[i], name) == 0)
			return;
	if (*n == *cap)
	{
		*cap *= 2;
		*names = repalloc_array(*names, char *, *cap);
	}
	(*names)[(*n)++] = pstrdup(name);
}

void
ws_role_read(const char *const *names, int n, WsRole **out, int *nout)
{
	int			level = WsSql->begin();
	int			cap = Max(n, 8);
	char	  **queue = palloc_array(char *, cap);
	int			nqueue = 0;
	WsRole	   *roles = palloc0_array(WsRole, cap);

	for (int i = 0; i < n; i++)
		add_name(&queue, &nqueue, &cap, names[i]);

	/* (every read of the one snapshot, or a role could change between them) */
	CommandCounterIncrement();
	PushCopiedSnapshot(GetTransactionSnapshot());
	UpdateActiveSnapshotCommandId();

	/* (the queue grows as memberships name more) */
	for (int q = 0; q < nqueue; q++)
	{
		const char *params[1] = {queue[q]};
		WsSqlRows	attr;
		WsSqlRows	members;
		WsRole	   *r = &roles[q];
		char	   *what = psprintf("role \"%s\"", queue[q]);
		const char *problem = NULL;

		if (IsReservedName(queue[q]))
			problem = "it is predefined";
		WsSql->read("SELECT r.rolinherit::text, r.rolcanlogin::text, "
					"(r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls)::text "
					"FROM pg_roles r WHERE r.rolname = $1::name", 1, params, &attr);
		if (problem == NULL && attr.nrows != 1)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", queue[q])));
		if (problem == NULL && ws_sql_value(&attr, 0, 1)[0] == 't')
			problem = "it can log in";
		if (problem == NULL && ws_sql_value(&attr, 0, 2)[0] == 't')
			problem = "it has a power no commit holds";
		if (problem != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("role \"%s\" cannot travel with a commit: %s", queue[q], problem),
					 errhint("Commits name roles that cannot log in and hold no powers of their own.")));
		ws_role_check_name(what, queue[q]);
		pfree(what);
		r->name = ws_name_to_utf8(queue[q]);
		r->no_inherit = ws_sql_value(&attr, 0, 0)[0] != 't';
		ws_sql_rows_free(&attr);

		WsSql->read("SELECT p.rolname::text, m.admin_option::text, m.inherit_option::text, m.set_option::text "
					"FROM pg_auth_members m JOIN pg_roles p ON p.oid = m.roleid "
					"JOIN pg_roles r ON r.oid = m.member WHERE r.rolname = $1::name", 1, params, &members);
		r->member_of = px_malloc((size_t) Max(members.nrows, 1) * sizeof(WsRoleMembership));
		memset(r->member_of, 0, (size_t) Max(members.nrows, 1) * sizeof(WsRoleMembership));
		r->nmember_of = members.nrows;
		for (int i = 0; i < members.nrows; i++)
		{
			const char *of = ws_sql_value(&members, i, 0);
			int			was = cap;

			add_name(&queue, &nqueue, &cap, of);
			if (cap != was)
			{
				roles = repalloc0_array(roles, WsRole, was, cap);
				r = &roles[q];
			}
			what = psprintf("role \"%s\"", of);
			ws_role_check_name(what, of);
			pfree(what);
			r->member_of[i].role = ws_name_to_utf8(of);
			r->member_of[i].admin = ws_sql_value(&members, i, 1)[0] == 't';
			r->member_of[i].inherit = ws_sql_value(&members, i, 2)[0] == 't';
			r->member_of[i].set = ws_sql_value(&members, i, 3)[0] == 't';
		}
		ws_sql_rows_free(&members);
		if (r->nmember_of > 1)
			qsort(r->member_of, r->nmember_of, sizeof(WsRoleMembership), membership_cmp);
		/* (who granted it is not held: granted twice, its options would be lost) */
		for (int i = 1; i < r->nmember_of; i++)
			if (strcmp(r->member_of[i - 1].role, r->member_of[i].role) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("role \"%s\" cannot travel with a commit: a membership of it was granted by more than one role",
								queue[q])));
	}
	if (nqueue > 1)
		qsort(roles, nqueue, sizeof(WsRole), role_cmp);
	PopActiveSnapshot();
	WsSql->end(level);
	*out = roles;
	*nout = nqueue;
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
ws_role_map(ProllyCtx *ctx, const WsRole *roles, int n, PHash *map)
{
	ProllyKV   *kvs = palloc_array(ProllyKV, Max(n, 1));
	Buf		   *keys = palloc0_array(Buf, Max(n, 1));
	Buf		   *vals = palloc0_array(Buf, Max(n, 1));
	int			rc;

	for (int i = 0; i < n; i++)
	{
		WsRoleCodec->encode_key(&roles[i], &keys[i]);
		WsRoleCodec->encode_value(&roles[i], &vals[i]);
		kvs[i].key = keys[i].data;
		kvs[i].klen = keys[i].len;
		kvs[i].val = vals[i].data;
		kvs[i].vlen = vals[i].len;
	}
	if (n > 1)
		qsort(kvs, n, sizeof(ProllyKV), kv_cmp);
	/* (a tree of two entries of one key is no tree a read would take) */
	for (int i = 1; i < n; i++)
		if (kv_cmp(&kvs[i - 1], &kvs[i]) == 0)
			elog(ERROR, "two roles of one name");
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
ws_role_list(ProllyCtx *ctx, const PHash *map, WsRole **out, int *n)
{
	ProllyIter *it;
	const uint8 *key;
	const uint8 *val;
	size_t		klen;
	size_t		vlen;
	uint64		count;
	int			rc;
	int			i = 0;

	rc = prolly_count(ctx, map, &count);
	if (rc < 0)
		prolly_report(ctx);
	*out = palloc0_array(WsRole, Max(count, 1));
	rc = prolly_iter_open(ctx, map, NULL, 0, &it);
	if (rc < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		/* (a root's count is as trusted as its entries, which is not at all) */
		if ((uint64) i >= count)
			malformed("a map of more keys than it says");
		WsRoleCodec->decode(key, klen, val, vlen, &(*out)[i++]);
	}
	if (rc < 0)
		prolly_report(ctx);
	prolly_iter_close(it);
	*n = i;
}

/* ------------------------------------------------------------- arriving */

static bool
role_among(const WsRole *roles, int n, const char *name)
{
	int			lo = 0,
				hi = n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			cmp = strcmp(name, roles[mid].name);

		if (cmp == 0)
			return true;
		if (cmp < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return false;
}

void
ws_role_check_arrival(const WsRole *roles, int n, const WsRole *known, int nknown)
{
	for (int i = 0; i < n; i++)
	{
		const WsRole *r = &roles[i];
		char	   *here;
		char	   *what;
		const char *problem;

		CHECK_FOR_INTERRUPTS();
		problem = ws_role_problem(r);
		if (problem != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a commit's role \"%s\": %s", r->name ? r->name : "", problem)));
		if (i > 0 && strcmp(roles[i - 1].name, r->name) >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a commit's roles not in order")));
		for (int k = 0; k < r->nmember_of; k++)
			if (!role_among(roles, n, r->member_of[k].role))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a commit's role \"%s\" is a member of \"%s\", which it does not hold",
								r->name, r->member_of[k].role)));
		here = ws_name_from_utf8(r->name);
		what = psprintf("a commit's role \"%s\"", here);
		ws_role_check_name(what, here);
		problem = ws_role_travel_problem(here);
		if (problem != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a commit's role \"%s\" cannot arrive here: %s", here, problem)));
		if (SearchSysCacheExists1(AUTHNAME, PointerGetDatum(here)) && !role_among(known, nknown, r->name))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a commit's role \"%s\" is here already, and not from this database", here),
					 errdetail("What the commit grants would reach that role's members.")));
		pfree(what);
		pfree(here);
	}
}

/* ------------------------------------------------------------ statements */

pg_noreturn static void
not_the_role(const char *name, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("role \"%s\" does not make that role: %s", name, what)));
}

/* The text as parse trees, or a refusal: it is the record's fault. */
static List *
parse_role_text(const char *name, const char *sql)
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
		not_the_role(name, edata->message);
	}
	PG_END_TRY();
	return parsed;
}

/* That the options of a role's statement are just these, and what they say. */
static void
check_role_options(const char *name, List *options, bool login_said, bool inherit_said,
				   bool inherit)
{
	ListCell   *lc;

	if (list_length(options) != (login_said ? 1 : 0) + (inherit_said ? 1 : 0))
		not_the_role(name, "another option");
	foreach(lc, options)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (!IsA(d->arg, Boolean))
			not_the_role(name, "another option");
		if (login_said && strcmp(d->defname, "canlogin") == 0 && !boolVal(d->arg))
			login_said = false;
		else if (inherit_said && strcmp(d->defname, "inherit") == 0 && boolVal(d->arg) == inherit)
			inherit_said = false;
		else
			not_the_role(name, "another option");
	}
}

void
ws_role_make_statement(const WsRole *r, Buf *out)
{
	const char *problem = ws_role_problem(r);
	char	   *here;
	Buf			sql = BUF_INIT;
	List	   *parsed;
	Node	   *node;
	HeapTuple	tup;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	here = ws_name_from_utf8(r->name);
	tup = SearchSysCache1(AUTHNAME, PointerGetDatum(here));
	if (HeapTupleIsValid(tup))
	{
		bool		same = ((Form_pg_authid) GETSTRUCT(tup))->rolinherit != r->no_inherit;

		ReleaseSysCache(tup);
		if (same)
		{
			pfree(here);
			return;
		}
		ws_put_text(&sql, "ALTER ROLE ");
		ws_put_identifier(&sql, here);
		ws_put_text(&sql, r->no_inherit ? " NOINHERIT" : " INHERIT");
		buf_append_byte(&sql, 0);
		parsed = parse_role_text(here, (char *) sql.data);
		node = list_length(parsed) == 1 ? linitial_node(RawStmt, parsed)->stmt : NULL;
		if (node == NULL || !IsA(node, AlterRoleStmt))
			not_the_role(here, "it is not one ALTER ROLE");
		if (((AlterRoleStmt *) node)->role->roletype != ROLESPEC_CSTRING ||
			strcmp(((AlterRoleStmt *) node)->role->rolename, here) != 0)
			not_the_role(here, "another role");
		check_role_options(here, ((AlterRoleStmt *) node)->options, false, true, !r->no_inherit);
	}
	else
	{
		ws_put_text(&sql, "CREATE ROLE ");
		ws_put_identifier(&sql, here);
		ws_put_text(&sql, r->no_inherit ? " NOLOGIN NOINHERIT" : " NOLOGIN");
		buf_append_byte(&sql, 0);
		parsed = parse_role_text(here, (char *) sql.data);
		node = list_length(parsed) == 1 ? linitial_node(RawStmt, parsed)->stmt : NULL;
		if (node == NULL || !IsA(node, CreateRoleStmt))
			not_the_role(here, "it is not one CREATE ROLE");
		if (((CreateRoleStmt *) node)->stmt_type != ROLESTMT_ROLE ||
			strcmp(((CreateRoleStmt *) node)->role, here) != 0)
			not_the_role(here, "another role");
		check_role_options(here, ((CreateRoleStmt *) node)->options, true, r->no_inherit, false);
	}
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
	pfree(here);
}

void
ws_role_grant_statement(const WsRole *r, int k, Buf *out)
{
	const char *problem = ws_role_problem(r);
	const WsRoleMembership *m;
	char	   *here;
	char	   *of;
	Buf			sql = BUF_INIT;
	List	   *parsed;
	GrantRoleStmt *stmt;
	AccessPriv *granted;
	RoleSpec   *grantee;
	static const char *const names[] = {"admin", "inherit", "set"};
	bool		wants[3];
	int			i = 0;
	ListCell   *lc;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (k < 0 || k >= r->nmember_of)
		elog(ERROR, "no membership %d", k);
	m = &r->member_of[k];
	wants[0] = m->admin;
	wants[1] = m->inherit;
	wants[2] = m->set;
	here = ws_name_from_utf8(r->name);
	of = ws_name_from_utf8(m->role);
	ws_put_text(&sql, "GRANT ");
	ws_put_identifier(&sql, of);
	ws_put_text(&sql, " TO ");
	ws_put_identifier(&sql, here);
	ws_put_text(&sql, m->admin ? " WITH ADMIN TRUE" : " WITH ADMIN FALSE");
	ws_put_text(&sql, m->inherit ? ", INHERIT TRUE" : ", INHERIT FALSE");
	ws_put_text(&sql, m->set ? ", SET TRUE" : ", SET FALSE");
	buf_append_byte(&sql, 0);
	parsed = parse_role_text(here, (char *) sql.data);
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, GrantRoleStmt))
		not_the_role(here, "it is not one GRANT");
	stmt = (GrantRoleStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (!stmt->is_grant || stmt->grantor != NULL || list_length(stmt->granted_roles) != 1 ||
		list_length(stmt->grantee_roles) != 1 || list_length(stmt->opt) != 3)
		not_the_role(here, "another grant");
	granted = linitial_node(AccessPriv, stmt->granted_roles);
	grantee = linitial_node(RoleSpec, stmt->grantee_roles);
	if (granted->cols != NIL || granted->priv_name == NULL || strcmp(granted->priv_name, of) != 0 ||
		grantee->roletype != ROLESPEC_CSTRING || strcmp(grantee->rolename, here) != 0)
		not_the_role(here, "another grant");
	foreach(lc, stmt->opt)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, names[i]) != 0 || !IsA(d->arg, Boolean) || boolVal(d->arg) != wants[i])
			not_the_role(here, "another grant");
		i++;
	}
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
	pfree(of);
	pfree(here);
}

/* (whether a membership is here as it is held: granting it again would add a row) */
static bool
membership_here(const char *role, const WsRoleMembership *m)
{
	char	   *of = ws_name_from_utf8(m->role);
	const char *params[5] = {of, role, m->admin ? "t" : "f", m->inherit ? "t" : "f", m->set ? "t" : "f"};
	WsSqlRows	rows;
	bool		here;

	WsSql->run("SELECT 1 FROM pg_auth_members m JOIN pg_roles p ON p.oid = m.roleid "
			   "JOIN pg_roles r ON r.oid = m.member WHERE p.rolname = $1::name AND r.rolname = $2::name "
			   "AND m.admin_option = $3::bool AND m.inherit_option = $4::bool AND m.set_option = $5::bool",
			   5, params, &rows);
	here = rows.nrows > 0;
	ws_sql_rows_free(&rows);
	pfree(of);
	return here;
}

void
ws_role_restore(const WsRole *roles, int n, const WsRole *known, int nknown)
{
	int			level;

	ws_role_check_arrival(roles, n, known, nknown);
	level = WsSql->begin();
	for (int i = 0; i < n; i++)
	{
		Buf			sql = BUF_INIT;

		ws_role_make_statement(&roles[i], &sql);
		if (sql.len > 0)
		{
			WsSqlRows	rows;

			buf_append_byte(&sql, 0);
			WsSql->run((char *) sql.data, 0, NULL, &rows);
			ws_sql_rows_free(&rows);
			CommandCounterIncrement();
		}
		buf_free(&sql);
	}
	for (int i = 0; i < n; i++)
	{
		char	   *here = ws_name_from_utf8(roles[i].name);

		for (int k = 0; k < roles[i].nmember_of; k++)
		{
			Buf			sql = BUF_INIT;
			WsSqlRows	rows;

			if (membership_here(here, &roles[i].member_of[k]))
				continue;
			ws_role_grant_statement(&roles[i], k, &sql);
			buf_append_byte(&sql, 0);
			WsSql->run((char *) sql.data, 0, NULL, &rows);
			ws_sql_rows_free(&rows);
			buf_free(&sql);
		}
		pfree(here);
	}
	WsSql->end(level);
}
