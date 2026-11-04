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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "storage/lmgr.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
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
	int			rc;
	int			cap = 8;
	int			i = 0;

	/* (grown as they are read: a root's count is not to be trusted with memory) */
	*out = palloc0_array(WsRole, cap);
	rc = prolly_iter_open(ctx, map, NULL, 0, &it);
	if (rc < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		if (i == cap)
		{
			*out = repalloc0_array(*out, WsRole, cap, cap * 2);
			cap *= 2;
		}
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

/* A membership's rows here, one a grantor. */
typedef struct HereRow
{
	Oid			grantor;
	bool		admin;
	bool		inherit;
	bool		set;
} HereRow;

/*
 * What a role is a member of here, or of of alone if of is valid: from the catalog
 * as GRANT will read it, not this transaction's snapshot, which may be old.
 */
static int
memberships_here(Oid member, Oid of, HereRow **rows, Oid **ofs)
{
	Relation	rel = table_open(AuthMemRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	int			cap = 4;
	int			n = 0;

	ScanKeyInit(&key[0], Anum_pg_auth_members_member, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(member));
	if (OidIsValid(of))
		ScanKeyInit(&key[1], Anum_pg_auth_members_roleid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(of));
	scan = systable_beginscan(rel, AuthMemMemRoleIndexId, true, NULL, OidIsValid(of) ? 2 : 1, key);
	if (rows != NULL)
		*rows = palloc_array(HereRow, cap);
	if (ofs != NULL)
		*ofs = palloc_array(Oid, cap);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_auth_members m = (Form_pg_auth_members) GETSTRUCT(tup);

		if (n == cap)
		{
			cap *= 2;
			if (rows != NULL)
				*rows = repalloc_array(*rows, HereRow, cap);
			if (ofs != NULL)
				*ofs = repalloc_array(*ofs, Oid, cap);
		}
		if (rows != NULL)
		{
			(*rows)[n].grantor = m->grantor;
			(*rows)[n].admin = m->admin_option;
			(*rows)[n].inherit = m->inherit_option;
			(*rows)[n].set = m->set_option;
		}
		if (ofs != NULL)
			(*ofs)[n] = m->roleid;
		n++;
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return n;
}

/* Whether a membership is here just as held: one row, of its options. */
static bool
membership_is_here(Oid member, Oid of, const WsRoleMembership *m)
{
	HereRow    *rows;
	int			n = memberships_here(member, of, &rows, NULL);
	bool		same = n == 1 && rows[0].admin == m->admin && rows[0].inherit == m->inherit &&
		rows[0].set == m->set;

	pfree(rows);
	return same;
}

/* Whether a membership gives more than what is here does, by any grantor. */
static bool
membership_widens(Oid member, Oid of, const WsRoleMembership *m)
{
	HereRow    *rows;
	int			n = memberships_here(member, of, &rows, NULL);
	bool		admin = false;
	bool		inherit = false;
	bool		set = false;

	for (int i = 0; i < n; i++)
	{
		admin |= rows[i].admin;
		inherit |= rows[i].inherit;
		set |= rows[i].set;
	}
	pfree(rows);
	return n == 0 || (m->admin && !admin) || (m->inherit && !inherit) || (m->set && !set);
}

/* Whether a role is here, marked, and one that can travel: a commit's to change. */
static bool
role_is_a_commits(const char *name)
{
	char	   *here = ws_name_from_utf8(name);
	HeapTuple	tup = SearchSysCache1(AUTHNAME, PointerGetDatum(here));
	bool		marked = false;

	if (HeapTupleIsValid(tup))
	{
		marked = ((Form_pg_authid) GETSTRUCT(tup))->roltravels;
		ReleaseSysCache(tup);
	}
	marked = marked && ws_role_travel_problem(here) == NULL;
	pfree(here);
	return marked;
}

/*
 * Whether a membership, in commit names, is one previous holds and roles do not, of
 * two roles a commit may change: nothing else is the commit's to remove.
 */
static bool
membership_removed(const WsRole *roles, int n, const WsRole *previous, int nprevious,
				   const char *member, const char *of)
{
	bool		held = false;

	for (int i = 0; i < nprevious && !held; i++)
		if (strcmp(previous[i].name, member) == 0)
			for (int k = 0; k < previous[i].nmember_of; k++)
				held |= strcmp(previous[i].member_of[k].role, of) == 0;
	if (!held)
		return false;
	for (int i = 0; i < n; i++)
		if (strcmp(roles[i].name, member) == 0)
			for (int k = 0; k < roles[i].nmember_of; k++)
				if (strcmp(roles[i].member_of[k].role, of) == 0)
					return false;
	return role_is_a_commits(member) && role_is_a_commits(of);
}

/*
 * Refuses a membership of of that would reach, by memberships here, a role that
 * roles do not hold: the role's members here would become members of it.
 */
static void
check_reach(const WsRole *roles, int n, const WsRole *previous, int nprevious, const char *role, Oid of)
{
	int			cap = 16;
	Oid		   *seen = palloc_array(Oid, cap);
	int			nseen = 0;

	seen[nseen++] = of;
	for (int q = 0; q < nseen; q++)
	{
		Oid		   *ofs;
		int			m = memberships_here(seen[q], InvalidOid, NULL, &ofs);
		char	   *from = GetUserNameFromId(seen[q], false);
		char	   *from_held = pstrdup(pg_server_to_any(from, (int) strlen(from), PG_UTF8));

		CHECK_FOR_INTERRUPTS();
		for (int i = 0; i < m; i++)
		{
			char	   *name = GetUserNameFromId(ofs[i], false);
			char	   *held = pstrdup(pg_server_to_any(name, (int) strlen(name), PG_UTF8));
			bool		dup = false;

			/* (the commit takes this one away before it adds anything) */
			if (membership_removed(roles, n, previous, nprevious, from_held, held))
			{
				pfree(held);
				continue;
			}
			if (!role_among(roles, n, held))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a commit's role \"%s\" would reach role \"%s\", which it does not hold",
								role, name),
						 errdetail("That role's members here would become members of it.")));
			pfree(held);
			for (int s = 0; s < nseen; s++)
				dup |= seen[s] == ofs[i];
			if (!dup)
			{
				if (nseen == cap)
				{
					cap *= 2;
					seen = repalloc_array(seen, Oid, cap);
				}
				seen[nseen++] = ofs[i];
			}
		}
		pfree(from_held);
		pfree(ofs);
	}
	pfree(seen);
}

void
ws_role_check_arrival(const WsRole *roles, int n, const WsRole *previous, int nprevious)
{
	for (int i = 0; i < n; i++)
	{
		const WsRole *r = &roles[i];
		char	   *here;
		char	   *what;
		const char *problem;
		HeapTuple	tup;

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
		tup = SearchSysCache1(AUTHNAME, PointerGetDatum(here));
		if (HeapTupleIsValid(tup))
		{
			bool		marked = ((Form_pg_authid) GETSTRUCT(tup))->roltravels;

			ReleaseSysCache(tup);
			if (!marked)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a commit's role \"%s\" is here already, and not held by any commit here", here),
						 errdetail("What the commit grants would reach that role's members.")));
		}
		pfree(what);
		pfree(here);
	}
	/* (once every role is known to be allowed: what a change here reaches) */
	for (int i = 0; i < n; i++)
	{
		char	   *here = ws_name_from_utf8(roles[i].name);
		Oid			member = get_role_oid(here, true);

		for (int k = 0; k < roles[i].nmember_of; k++)
		{
			char	   *name = ws_name_from_utf8(roles[i].member_of[k].role);
			Oid			of = get_role_oid(name, true);

			if (OidIsValid(of) &&
				(!OidIsValid(member) || membership_widens(member, of, &roles[i].member_of[k])))
				check_reach(roles, n, previous, nprevious, here, of);
			pfree(name);
		}
		pfree(here);
	}
}

/* (the role's lock, as AddRoleMems takes it, for one marking at a time) */
static void
mark_role(Relation rel, const char *here)
{
	Oid			oid = get_role_oid(here, false);
	HeapTuple	tup;
	Form_pg_authid form;
	const char *problem = ws_role_travel_problem(here);

	if (problem != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("role \"%s\" cannot travel with a commit: %s", here, problem)));
	LockSharedObject(AuthIdRelationId, oid, 0, ShareUpdateExclusiveLock);
	tup = SearchSysCacheCopy1(AUTHOID, ObjectIdGetDatum(oid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", here)));
	form = (Form_pg_authid) GETSTRUCT(tup);
	if (!form->roltravels)
	{
		form->roltravels = true;
		CatalogTupleUpdate(rel, &tup->t_self, tup);
	}
	heap_freetuple(tup);
}

void
ws_role_mark(const WsRole *roles, int n)
{
	Relation	rel = table_open(AuthIdRelationId, RowExclusiveLock);

	for (int i = 0; i < n; i++)
	{
		char	   *here = ws_name_from_utf8(roles[i].name);
		HeapTuple	tup = SearchSysCache1(AUTHNAME, PointerGetDatum(here));
		bool		marked;

		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", here)));
		marked = ((Form_pg_authid) GETSTRUCT(tup))->roltravels;
		ReleaseSysCache(tup);
		/* (nothing to do takes no right and no lock: capture does this each time) */
		if (!marked)
		{
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to let role \"%s\" travel with a commit", here),
						 errdetail("Only a superuser may do this, as a commit may then change that role.")));
			mark_role(rel, here);
		}
		pfree(here);
	}
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();
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
		Form_pg_authid form = (Form_pg_authid) GETSTRUCT(tup);
		bool		same = form->rolinherit != r->no_inherit;
		bool		marked = form->roltravels;

		ReleaseSysCache(tup);
		/* (what arrival allowed may have changed since: this is what acts on it) */
		if (!marked || ws_role_travel_problem(here) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("role \"%s\" is here, and not one a commit may change", here)));
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
ws_role_grant_statement(const WsRole *r, int k, const char *grantor, Buf *out)
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
	if (grantor != NULL)
	{
		ws_put_text(&sql, " GRANTED BY ");
		ws_put_identifier(&sql, grantor);
	}
	buf_append_byte(&sql, 0);
	parsed = parse_role_text(here, (char *) sql.data);
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, GrantRoleStmt))
		not_the_role(here, "it is not one GRANT");
	stmt = (GrantRoleStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (!stmt->is_grant ||
		(grantor == NULL ? stmt->grantor != NULL :
		 stmt->grantor == NULL || stmt->grantor->roletype != ROLESPEC_CSTRING ||
		 strcmp(stmt->grantor->rolename, grantor) != 0) ||
		list_length(stmt->granted_roles) != 1 ||
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

void
ws_role_revoke_statement(const char *member, const char *of, const char *grantor, Buf *out)
{
	Buf			sql = BUF_INIT;
	List	   *parsed;
	GrantRoleStmt *stmt;

	ws_put_text(&sql, "REVOKE ");
	ws_put_identifier(&sql, of);
	ws_put_text(&sql, " FROM ");
	ws_put_identifier(&sql, member);
	ws_put_text(&sql, " GRANTED BY ");
	ws_put_identifier(&sql, grantor);
	buf_append_byte(&sql, 0);
	parsed = parse_role_text(member, (char *) sql.data);
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, GrantRoleStmt))
		not_the_role(member, "it is not one REVOKE");
	stmt = (GrantRoleStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->is_grant || stmt->opt != NIL || list_length(stmt->granted_roles) != 1 ||
		list_length(stmt->grantee_roles) != 1 ||
		strcmp(linitial_node(AccessPriv, stmt->granted_roles)->priv_name, of) != 0 ||
		linitial_node(RoleSpec, stmt->grantee_roles)->roletype != ROLESPEC_CSTRING ||
		strcmp(linitial_node(RoleSpec, stmt->grantee_roles)->rolename, member) != 0 ||
		stmt->grantor == NULL || stmt->grantor->roletype != ROLESPEC_CSTRING ||
		strcmp(stmt->grantor->rolename, grantor) != 0)
		not_the_role(member, "another revoke");
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}

static void
run_role_statement(Buf *sql)
{
	WsSqlRows	rows;

	buf_append_byte(sql, 0);
	WsSql->run((char *) sql->data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	CommandCounterIncrement();
	buf_free(sql);
}

/*
 * The first role a role has passed a membership of of to here, or InvalidOid: a
 * commit has no say in that grant.
 */
static Oid
dependent_of(Oid member_oid, Oid of_oid)
{
	Relation	rel = table_open(AuthMemRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			found = InvalidOid;

	ScanKeyInit(&key, Anum_pg_auth_members_roleid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(of_oid));
	scan = systable_beginscan(rel, AuthMemRoleMemIndexId, true, NULL, 1, &key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_auth_members m = (Form_pg_auth_members) GETSTRUCT(tup);

		if (m->grantor == member_oid)
		{
			found = m->member;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/* Refuses by name, as an error of the server's would offer a CASCADE no one pulling can use. */
static void
check_no_dependents(const char *member, Oid member_oid, const char *of, Oid of_oid)
{
	Oid			dependent = dependent_of(member_oid, of_oid);

	if (OidIsValid(dependent))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("a commit takes the ADMIN option on role \"%s\" from role \"%s\", which granted it here to role \"%s\"",
						of, member, GetUserNameFromId(dependent, false)),
				 errhint("Revoke that grant here before the commit arrives.")));
}

/* Revokes what is here of a membership, whoever granted it, but for one row's grantor. */
static void
revoke_membership(const char *member, const char *of, Oid keep)
{
	HereRow    *rows;
	Oid			member_oid = get_role_oid(member, false);
	Oid			of_oid = get_role_oid(of, false);
	int			n = memberships_here(member_oid, of_oid, &rows, NULL);

	for (int i = 0; i < n; i++)
	{
		Buf			sql = BUF_INIT;

		if (rows[i].grantor == keep)
			continue;
		ws_role_revoke_statement(member, of, GetUserNameFromId(rows[i].grantor, false), &sql);
		run_role_statement(&sql);
	}
	pfree(rows);
}

/*
 * The row of a membership to change in place, or -1: one whose grantor may still
 * grant it.  A member is no grantor to itself, and one that has lost its ADMIN can
 * only be replaced.
 */
static int
row_to_keep(const HereRow *rows, int n, Oid member, Oid of)
{
	for (int i = 0; i < n; i++)
		if (rows[i].grantor != member &&
			(superuser_arg(rows[i].grantor) || is_admin_of_role(rows[i].grantor, of)))
			return i;
	return -1;
}

/*
 * Leaves a membership as held: in place if it can be, as a row of another grantor
 * would keep what this one takes away, and revoking would take with it what depends
 * on it.
 */
static void
set_membership(const WsRole *r, int k, const char *member)
{
	const WsRoleMembership *m = &r->member_of[k];
	char	   *of = ws_name_from_utf8(m->role);
	Oid			member_oid = get_role_oid(member, false);
	Oid			of_oid = get_role_oid(of, false);
	HereRow    *rows;
	int			n;
	int			keep;
	bool		admin = false;
	Buf			sql = BUF_INIT;

	/* (as AddRoleMems does: no grant by this role can come while this looks for them) */
	LockSharedObject(AuthIdRelationId, of_oid, 0, ShareUpdateExclusiveLock);
	if (membership_is_here(member_oid, of_oid, m))
	{
		pfree(of);
		return;
	}
	n = memberships_here(member_oid, of_oid, &rows, NULL);
	keep = row_to_keep(rows, n, member_oid, of_oid);
	for (int i = 0; i < n; i++)
		admin |= rows[i].admin;
	if (admin && (!m->admin || keep < 0))
		check_no_dependents(member, member_oid, of, of_oid);
	if (keep >= 0)
	{
		/* (a grant of what it already says would only say so, in a NOTICE) */
		if (rows[keep].admin != m->admin || rows[keep].inherit != m->inherit || rows[keep].set != m->set)
		{
			ws_role_grant_statement(r, k, GetUserNameFromId(rows[keep].grantor, false), &sql);
			run_role_statement(&sql);
		}
		revoke_membership(member, of, rows[keep].grantor);
	}
	else
	{
		revoke_membership(member, of, InvalidOid);
		ws_role_grant_statement(r, k, NULL, &sql);
		run_role_statement(&sql);
	}
	buf_free(&sql);
	pfree(rows);
	pfree(of);
}

/*
 * Revokes what the commit removed, each once what it has passed on is gone, which
 * may be by another removal, in whatever order their names are.
 */
static void
remove_memberships(const WsRole *roles, int n, const WsRole *previous, int nprevious)
{
	bool		progress = true;
	bool		pending = true;

	while (pending && progress)
	{
		pending = false;
		progress = false;
		for (int i = 0; i < nprevious; i++)
			for (int k = 0; k < previous[i].nmember_of; k++)
			{
				const char *of = previous[i].member_of[k].role;
				char	   *here;
				char	   *of_here;
				Oid			member_oid;
				Oid			of_oid;
				Oid		   *ofs;

				CHECK_FOR_INTERRUPTS();
				if (!membership_removed(roles, n, previous, nprevious, previous[i].name, of))
					continue;
				here = ws_name_from_utf8(previous[i].name);
				of_here = ws_name_from_utf8(of);
				member_oid = get_role_oid(here, false);
				of_oid = get_role_oid(of_here, false);
				if (memberships_here(member_oid, of_oid, NULL, &ofs) > 0)
				{
					LockSharedObject(AuthIdRelationId, of_oid, 0, ShareUpdateExclusiveLock);
					if (OidIsValid(dependent_of(member_oid, of_oid)))
						pending = true;
					else
					{
						revoke_membership(here, of_here, InvalidOid);
						progress = true;
					}
				}
				pfree(ofs);
				pfree(of_here);
				pfree(here);
			}
	}
	if (!pending)
		return;
	/* (what is left depends on what no commit removes: say which) */
	for (int i = 0; i < nprevious; i++)
		for (int k = 0; k < previous[i].nmember_of; k++)
			if (membership_removed(roles, n, previous, nprevious, previous[i].name,
								   previous[i].member_of[k].role))
			{
				char	   *here = ws_name_from_utf8(previous[i].name);
				char	   *of_here = ws_name_from_utf8(previous[i].member_of[k].role);

				check_no_dependents(here, get_role_oid(here, false), of_here, get_role_oid(of_here, false));
				pfree(of_here);
				pfree(here);
			}
}

void
ws_role_restore(const WsRole *roles, int n, const WsRole *previous, int nprevious)
{
	int			level;
	Relation	rel;

	ws_role_check_arrival(roles, n, previous, nprevious);
	level = WsSql->begin();
	for (int i = 0; i < n; i++)
	{
		Buf			sql = BUF_INIT;

		ws_role_make_statement(&roles[i], &sql);
		if (sql.len > 0)
			run_role_statement(&sql);
		buf_free(&sql);
	}

	/* (arrival allowed these: none here was not marked, and the rest it just made) */
	rel = table_open(AuthIdRelationId, RowExclusiveLock);
	for (int i = 0; i < n; i++)
	{
		char	   *here = ws_name_from_utf8(roles[i].name);

		mark_role(rel, here);
		pfree(here);
	}
	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();

	/* (what a commit removed first, so that it may reverse a membership) */
	remove_memberships(roles, n, previous, nprevious);
	for (int i = 0; i < n; i++)
	{
		char	   *here = ws_name_from_utf8(roles[i].name);

		for (int k = 0; k < roles[i].nmember_of; k++)
			set_membership(&roles[i], k, here);
		pfree(here);
	}
	WsSql->end(level);
}
