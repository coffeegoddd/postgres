/*-------------------------------------------------------------------------
 *
 * ws_role.h
 *	  The roles that travel with a versioned schema, as a root value holds them.
 *
 * Who may do what belongs to a database, and who may log in to a cluster.  So only
 * group roles travel: those that cannot log in, with no power beyond what they are
 * granted, and which of each other they are members of.  A table's owner and
 * grants name them (ws_schema.h).  A role that can log in is never in a commit, and
 * nor is anything else of a role but this: no password, expiry, connection limit or
 * setting, all of which stay on the cluster.  Someone gets a role's access by a
 * membership made there, which stays there too.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_role.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_ROLE_H
#define PROLLY_WS_ROLE_H

#include "prolly/engine/prolly.h"

/* That a role is a member of another, and how. */
typedef struct WsRoleMembership
{
	char	   *role;
	bool		admin;
	bool		inherit;
	bool		set;
} WsRoleMembership;

typedef struct WsRole
{
	char	   *name;
	bool		no_inherit;		/* NOINHERIT */
	int			nmember_of;
	WsRoleMembership *member_of;	/* by name in UTF8's order */
} WsRole;

/*
 * What a role must be: named, in names Postgres would not shorten and could make,
 * not predefined, a member of others in order, and not of itself.  NULL if it is
 * so, or what is wrong.
 */
extern const char *ws_role_problem(const WsRole *r);

/*
 * The codec: key is its name, value the rest.  The same role is always the same
 * bytes, and only those are read as it; decode raises ERRCODE_DATA_CORRUPTED
 * otherwise, encode an error if it has a problem.  What decode gives is the
 * caller's to ws_role_free.
 */
typedef struct WsRoleCodecRoutine
{
	const char *name;
	void		(*encode_key) (const WsRole *r, Buf *out);
	void		(*encode_value) (const WsRole *r, Buf *out);
	void		(*decode) (const uint8 *key, size_t klen, const uint8 *val, size_t vlen,
						   WsRole *out);
} WsRoleCodecRoutine;

extern PGDLLIMPORT const WsRoleCodecRoutine BinaryWsRoleCodec;

/* The implementation in use. */
extern PGDLLIMPORT const WsRoleCodecRoutine *WsRoleCodec;

extern void ws_role_free(WsRole *r);

/*
 * Why a role, by its name here, cannot travel, or NULL if it can or does not exist:
 * it can log in, has a power no commit holds, or is predefined.
 */
extern const char *ws_role_travel_problem(const char *role);

/*
 * Raises ERRCODE_FEATURE_NOT_SUPPORTED if a name not in ASCII may not mean the same
 * here: roles are shared by a cluster's databases and a name's bytes are in the
 * encoding of whichever made it, so it may be another name if their encodings differ,
 * or not this encoding at all if one that did has gone.  what names what names it.
 */
extern void ws_role_check_name(const char *what, const char *role);

/*
 * The named roles, in this database's encoding, and all they are members of, from
 * the catalogs, through WsSql under one snapshot, by name in UTF8's order, in *out,
 * palloc'd, each the caller's to ws_role_free.  Raises
 * ERRCODE_FEATURE_NOT_SUPPORTED for one that cannot travel, or a membership granted
 * by more than one role, which is not held, and ERRCODE_UNDEFINED_OBJECT for one
 * that does not exist.
 */
extern void ws_role_read(const char *const *names, int n, WsRole **out, int *nout);

/* The ~roles map of these, its root in *map: their order does not matter. */
extern void ws_role_map(ProllyCtx *ctx, const WsRole *roles, int n, PHash *map);

/* The roles of a ~roles map, in *out, palloc'd, each the caller's to ws_role_free. */
extern void ws_role_list(ProllyCtx *ctx, const PHash *map, WsRole **out, int *n);

/*
 * Marks roles here as held by commits (pg_authid.roltravels), so that a commit may
 * later change them: what capture does with what ws_role_read gave it, once it is
 * allowed to write.  Marking one not yet marked takes ADMIN on it, as a mark lets a
 * commit say who is a member of it: ERRCODE_INSUFFICIENT_PRIVILEGE otherwise.  The
 * mark goes with the role.  A mark says only that some versioned database of this
 * cluster holds it, and those that hold one name share it.
 */
extern void ws_role_mark(const WsRole *roles, int n);

/*
 * Whether roles from a commit may arrive here: raises ERRCODE_DATA_CORRUPTED if one
 * is a member of a role not among them, and ERRCODE_FEATURE_NOT_SUPPORTED if one is
 * here already and not marked, as what the commit grants would reach that role's
 * members; if one here cannot travel; or if a membership it would add or widen
 * reaches, by memberships made here, a role not among them, which would give the
 * members of its role what no commit could hold.  roles is in order.  To call
 * before making anything.
 */
extern void ws_role_check_arrival(const WsRole *roles, int n);

/*
 * The statement making a role here as it is held, appended to out: a CREATE ROLE, an
 * ALTER ROLE if it is here otherwise, or nothing.  The GRANT making it a member of
 * its kth, to run once they are all made.  The REVOKE of a membership of of, by a
 * grantor.  Each checked by the grammar, as a table's statement is: anything else
 * raises ERRCODE_DATA_CORRUPTED.
 */
extern void ws_role_make_statement(const WsRole *r, Buf *out);
extern void ws_role_grant_statement(const WsRole *r, int k, Buf *out);
extern void ws_role_revoke_statement(const char *member, const char *of, const char *grantor, Buf *out);

/*
 * Makes roles from a commit here, through WsSql, once ws_role_check_arrival has
 * allowed them, each before any membership, and marks them.  A membership is left
 * exactly as held, whoever granted what was here.  One that previous, the roles of
 * the root this database was on, held and these do not is revoked if these hold
 * both its roles: the commit removed it.  One made here is not, and no role is ever
 * dropped, as this cluster's other databases may use it.  previous is in order.
 */
extern void ws_role_restore(const WsRole *roles, int n, const WsRole *previous, int nprevious);

#endif							/* PROLLY_WS_ROLE_H */
