/*-------------------------------------------------------------------------
 *
 * ws_obj.h
 *	  What a root value holds of a versioned schema's objects other than tables:
 *	  each one's definition as the statement that makes it, what it needs made
 *	  before it, and its comments, owner and grants.
 *
 * A definition is generated from the catalogs, not kept from what the user wrote,
 * which would go stale as objects change in place.  It is what follows the object's
 * name in the statement making it, and is written and run with the schema on the
 * search path after the catalogs, so it names what is in that schema without saying
 * which schema that is: a commit can be restored into any.  What an object needs
 * from any other schema but the catalogs is refused, and so is what would have to be
 * written with its schema's name, being named as something in the catalogs is.
 *
 * What is made before an object is named rather than found by trying, as the order
 * of a restore must not depend on what fails.
 *
 * A view has an owner that can travel, as it reads with its owner's rights: any other
 * owner could show other rows.  A type's owner that cannot is not held, and whoever
 * restores it owns it, as a table's does.  A commit's own definitions are trusted, as a
 * remote is, as what a pg_dump wrote is: they are checked only to be what they say.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_obj.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_OBJ_H
#define PROLLY_WS_OBJ_H

#include "prolly/engine/prolly.h"

typedef enum WsObjectKind
{
	WS_OBJ_DOMAIN,
	WS_OBJ_MATVIEW,
	WS_OBJ_TYPE,				/* an enum, composite or range type */
	WS_OBJ_VIEW,
} WsObjectKind;

/* A comment, on the object or one of its columns. */
typedef struct WsObjectComment
{
	char	   *column;			/* NULL: the object's */
	char	   *text;
} WsObjectComment;

/* What an object's owner has granted, as ws_schema.h holds a table's. */
typedef struct WsObjectGrant
{
	char	   *column;			/* NULL: the object's */
	char	   *grantee;		/* NULL: PUBLIC */
	bool		grant_option;
	char	   *privileges;		/* aclitem letters, in order by byte */
} WsObjectGrant;

typedef struct WsObject
{
	WsObjectKind kind;
	char	   *name;
	char	   *definition;		/* its statement after its name, as stored text (ws_tags.h) */
	int			ndeps;
	char	  **deps;			/* the keys of the objects it needs, in order */
	char	   *owner;			/* NULL: whoever restores it, for what may have none */
	int			ncomments;
	WsObjectComment *comments;	/* in order: the object's, then by column */
	int			ngrants;
	WsObjectGrant *grants;		/* in order: by column, grantee, grant option */
} WsObject;

/*
 * What an object must be: NULL if it is so, or what is wrong.  One that a catalog
 * could not hold is not one, and nor is one that names itself or a key of no kind.
 */
extern const char *ws_obj_problem(const WsObject *o);

/*
 * The codec: key is its kind and name, value the rest.  The same object is always
 * the same bytes, and only those are read as it; decode raises
 * ERRCODE_DATA_CORRUPTED otherwise, encode an error if it has a problem.  What
 * decode gives is the caller's to ws_obj_free.
 */
typedef struct WsObjectCodecRoutine
{
	const char *name;
	void		(*encode_key) (const WsObject *o, Buf *out);
	void		(*encode_value) (const WsObject *o, Buf *out);
	void		(*decode) (const uint8 *key, size_t klen, const uint8 *val, size_t vlen,
						   WsObject *out);
} WsObjectCodecRoutine;

extern PGDLLIMPORT const WsObjectCodecRoutine BinaryWsObjectCodec;

/* The implementation in use. */
extern PGDLLIMPORT const WsObjectCodecRoutine *WsObjectCodec;

/* Whether a kind is made before a commit's tables, which may use it, or after. */
extern bool ws_obj_before_tables(WsObjectKind kind);

extern void ws_obj_free(WsObject *o);

/* The key of an object of a kind and name, UTF8, appended to out. */
extern void ws_obj_key(WsObjectKind kind, const char *name, Buf *out);

/*
 * The objects of a schema from the catalogs as they are now, each locked, with what
 * it needs, until the transaction ends; through WsSql, in key order, in *out,
 * palloc'd, each the caller's to ws_obj_free.  A transaction on a snapshot the
 * schema's relations have changed since raises ERRCODE_T_R_SERIALIZATION_FAILURE, as
 * ws_schema_read does, to try again, so that tables and objects go together (but for
 * the names of other objects, as ws_schema.h says).  An
 * object it cannot hold raises ERRCODE_FEATURE_NOT_SUPPORTED, naming it: nothing is
 * left out of a commit in silence.
 */
extern void ws_obj_read(const char *schema, WsObject **out, int *n);

/* The ~obj map of these, its root in *map: their order does not matter. */
extern void ws_obj_map(ProllyCtx *ctx, const WsObject *objs, int n, PHash *map);

/* The objects of a ~obj map, in key order, in *out, palloc'd, each to ws_obj_free. */
extern void ws_obj_list(ProllyCtx *ctx, const PHash *map, WsObject **out, int *n);

/*
 * The order to make them in: order[i] the index of the ith, each after all it
 * needs.  One order for one set of objects, whatever it was reached by.  An object
 * needing one not among them, or a cycle of them, raises ERRCODE_DATA_CORRUPTED.
 * objs is in key order.
 */
extern void ws_obj_restore_order(const WsObject *objs, int n, int *order);

/*
 * The statement making an object, appended to out, checked by the grammar to be
 * only that: ERRCODE_DATA_CORRUPTED otherwise.
 */
extern void ws_obj_create_statement(const WsObject *o, const char *schema, Buf *out);

/* The statements of its comments, owner and grants, to run after it is made, in schema. */
extern void ws_obj_comment_statement(const WsObject *o, int k, const char *schema, Buf *out);
extern void ws_obj_owner_statement(const WsObject *o, const char *schema, Buf *out);
extern void ws_obj_grant_statement(const WsObject *o, int k, const char *schema, Buf *out);

/*
 * Makes the objects in schema that go before its tables, or after, through WsSql, in
 * restore order, each with its comments, then its owner, then its grants.  objs is
 * all of a commit's, as what comes after may need what comes before.  Raises
 * ERRCODE_FEATURE_NOT_SUPPORTED if default privileges would make them other than they
 * are held, or if the schema holds a function or operator named as one in the
 * catalogs, as a definition's unqualified name could be taken for it.  Who restores them must be able to SET ROLE
 * to each owner, with its privileges, and each owner must have CREATE on the schema,
 * as the server asks of any change of owner.
 */
extern void ws_obj_restore(const WsObject *objs, int n, const char *schema, bool before_tables);

/* Whether a type's name, unqualified, is one of those whose values are names. */
extern bool ws_is_reg_type(const char *type);

/* Whether an expression as the catalogs print it says a schema's name, by the grammar. */
extern bool ws_expression_names_schema(const char *expression, const char *schema);

#endif							/* PROLLY_WS_OBJ_H */
