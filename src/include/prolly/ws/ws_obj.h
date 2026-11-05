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
 * A trigger, rule or policy is of its table, whose name is in its key, and has no owner
 * or grants of its own, as is a materialized view's index and a view column's default.
 *
 * A cast is in no schema: it is held if its source, target or function is of this one.
 * What needs one says so only if by its function, so a cast comes as early as what it
 * needs lets it, its key sorting before functions'.
 *
 * Every family is held, implicit or not, and a class always says its family, so which
 * it was cannot differ.  An operator's commutator and negator may not exist yet when it
 * is made, and a name not found would be made as a shell in the path's first schema, so
 * they are set by what comes after both.
 *
 * What is made before an object is named rather than found by trying, as the order
 * of a restore must not depend on what fails.
 *
 * A view has an owner that can travel, as it reads with its owner's rights: any other
 * owner could show other rows.  A type's owner that cannot is not held, and whoever
 * restores it owns it, as a table's does.  So too a function's, but for one that is
 * SECURITY DEFINER, which reads with its owner's rights as a view does.  A function's
 * body in text is not looked into: if it names its schema, it says so as written, and
 * if it is inlined while a table is made, as an IMMUTABLE one may be in a generated
 * column or partition key, it must find there what it names.  A
 * commit's own definitions are trusted, as a
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
	WS_OBJ_CAST,				/* its source type as its name, its target as its args */
	WS_OBJ_COLLATION,
	WS_OBJ_CONVERSION,
	WS_OBJ_OPERATOR,			/* its input types as its args */
	WS_OBJ_OPLINK,				/* an operator's commutator and negator, as its args */
	WS_OBJ_OPCLASS,				/* its access method as its args, as of the next two */
	WS_OBJ_OPFAMILY,
	WS_OBJ_OPMEMBERS,			/* what a family holds outside any class */
	WS_OBJ_AGGREGATE,			/* its input types as its args, as a function's */
	WS_OBJ_TSPARSER,
	WS_OBJ_TSTEMPLATE,
	WS_OBJ_TSDICT,
	WS_OBJ_TSCONFIG,
	WS_OBJ_TSMAP,				/* a configuration's mapping of one token, as its args */
	WS_OBJ_STATISTICS,
	WS_OBJ_DEFAULT,				/* a view column's: its name, and its view */
	WS_OBJ_DOMAIN,
	WS_OBJ_FUNCTION,
	WS_OBJ_INDEX,				/* a materialized view's: a table's is in its record */
	WS_OBJ_MATVIEW,
	WS_OBJ_POLICY,
	WS_OBJ_PROCEDURE,
	WS_OBJ_RULE,
	WS_OBJ_SEQUENCE,			/* not an identity's, which its table's record holds */
	WS_OBJ_TRIGGER,
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

/*
 * A column a USING merges.  A USING can only say a name, as each side's column has its own
 * tag, so the tag it meant is held beside it, and a restore that finds the name is now
 * another column's refuses, rather than joining on that.
 */
typedef struct WsObjectUsing
{
	char	   *table;
	char	   *column;
	uint64		tag;
} WsObjectUsing;

/*
 * A table whose columns an alias list goes by the place of: what prints a list of its, or of a
 * join over it.  The tags of its columns in order are held beside it, and a restore that finds
 * them otherwise refuses, rather than reading another column.
 */
typedef struct WsObjectPlaces
{
	char	   *table;
	char	   *columns;		/* its columns' tags in order, comma-separated */
} WsObjectPlaces;

typedef struct WsObject
{
	WsObjectKind kind;
	char	   *name;
	char	   *args;			/* a function's input types, as printed, or "" if none; a
								 * trigger's, rule's or policy's table; else NULL */
	char	   *definition;		/* its statement after its name, as stored text (ws_tags.h);
								 * all of it for a trigger, rule or policy, naming its table bare */
	bool		after_tables;	/* a function that needs one, so made after them */
	bool		public_revoked; /* what PUBLIC would have is revoked from it */
	char		fires;			/* a trigger's or rule's ALTER TABLE state: 0, as made, or 'D',
								 * 'R' or 'A' */
	char	   *owned_table;	/* a sequence's OWNED BY, or NULL */
	char	   *owned_column;
	int			ndeps;
	char	  **deps;			/* the keys of the objects it needs, in order */
	char	   *owner;			/* NULL: whoever restores it, for what may have none */
	int			ncomments;
	WsObjectComment *comments;	/* in order: the object's, then by column */
	int			ngrants;
	WsObjectGrant *grants;		/* in order: by column, grantee, grant option */
	int			nusings;
	WsObjectUsing *usings;		/* in order: by table, column, tag */
	int			nplaces;
	WsObjectPlaces *places;		/* in order, by table */
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

/* Whether an object is made before a commit's tables, which may use it, or after. */
extern bool ws_obj_before_tables(const WsObject *o);

extern void ws_obj_free(WsObject *o);

/* The key of an object of a kind, name and args (NULL but for a function), UTF8, appended to out. */
extern void ws_obj_key(WsObjectKind kind, const char *name, const char *args, Buf *out);

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

/*
 * Where each of a schema's sequences is, an identity's too, as ~seq holds it (a
 * sequence's position is state, not its definition): by name in UTF8's order, in *out,
 * palloc'd.  It runs no user code to read or set them, and neither reads by a snapshot,
 * as a sequence does not: a capture must keep the schema from changing between this and
 * ws_obj_read, or a position may not suit the definition held.  It takes SELECT on each.
 */
typedef struct WsSeqPosition
{
	char	   *name;
	int64		last_value;
	bool		is_called;
} WsSeqPosition;

extern void ws_seq_read(const char *schema, WsSeqPosition **out, int *n);
extern void ws_seq_map(ProllyCtx *ctx, const WsSeqPosition *positions, int n, PHash *map);
extern void ws_seq_list(ProllyCtx *ctx, const PHash *map, WsSeqPosition **out, int *n);
extern void ws_seq_restore(const WsSeqPosition *positions, int n, const char *schema);

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
 * After objs are restored in schema, remembers what each now prints as if not as the commit
 * holds it, so that a read holds the commit's text while it does (prolly/ws/ws_print.h).
 * It reads the whole schema once.
 */
extern void ws_obj_remember(const WsObject *objs, int n, const char *schema);

/*
 * The statement making an object, appended to out, checked by the grammar to be
 * only that: ERRCODE_DATA_CORRUPTED otherwise.
 */
extern void ws_obj_create_statement(const WsObject *o, const char *schema, Buf *out);

/* The statements of its comments, owner and grants, to run after it is made, in schema. */
extern void ws_obj_comment_statement(const WsObject *o, int k, const char *schema, Buf *out);
extern void ws_obj_owner_statement(const WsObject *o, const char *schema, Buf *out);
extern void ws_obj_grant_statement(const WsObject *o, int k, const char *schema, Buf *out);
extern void ws_obj_owned_statement(const WsObject *o, const char *schema, Buf *out);

/*
 * The roles an object names, as a commit holds them, for what collects ~roles: its owner,
 * who it grants to, and who a policy is for, which only its statement says.  Appended to
 * *names, of *n and palloc'd or NULL if none yet.
 */
extern void ws_obj_roles(const WsObject *o, char ***names, int *n);

/*
 * Makes the objects in schema that go before its tables, or after their rows are in, as
 * COPY would fire a trigger, through WsSql, in restore order, each with its comments, then its owner, then its grants.  A sequence
 * is made before, and owned by its column after, which needs the table's owner its own.  objs is
 * all of a commit's, as what comes after may need what comes before.  Raises
 * ERRCODE_FEATURE_NOT_SUPPORTED if default privileges would make them other than they
 * are held, or if the schema holds a function or operator named as one in the
 * catalogs, as a definition's unqualified name could be taken for it.  Who restores them must be able to SET ROLE
 * to each owner, and have its privileges if it has grants (ERRCODE_INSUFFICIENT_PRIVILEGE
 * otherwise), and each owner must have CREATE on the schema, as the server asks of any
 * change of owner.
 */
extern void ws_obj_restore(const WsObject *objs, int n, const char *schema, bool before_tables);

/* Whether a type's name, unqualified, is one of those whose values are names. */
extern bool ws_is_reg_type(const char *type);

/* Whether an expression as the catalogs print it says a schema's name, by the grammar. */
extern bool ws_expression_names_schema(const char *expression, const char *schema);

#endif							/* PROLLY_WS_OBJ_H */
