/*-------------------------------------------------------------------------
 *
 * ws_schema.h
 *	  The working-set model's table schemas: a table's definition as
 *	  structured data, the bytes of its schema chunk.
 *
 * A column has a tag, given when it first appears, which survives renames
 * and retypes: rows, and everything else that refers to a column, name it by
 * tag.  Its type is format_type's text under the settings of a statement's
 * scope (prolly/ws/ws_sql.h), which names no cluster-local object; its
 * default or generation expression is stored text (prolly/ws/ws_tags.h),
 * referring to columns by tag.  Names are as they are, not quoted.  Columns
 * are in attribute order; the primary key has its constraint's name, and
 * lists its columns' tags in key order.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_schema.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_SCHEMA_H
#define PROLLY_WS_SCHEMA_H

#include "prolly/engine/util.h"
#include "prolly/ws/ws_row.h"

#define WS_COL_STORED 's'
#define WS_COL_GENERATED_STORED 'g'
#define WS_COL_GENERATED_VIRTUAL 'v'

#define WS_IDENTITY_NONE '\0'
#define WS_IDENTITY_ALWAYS 'a'
#define WS_IDENTITY_BY_DEFAULT 'd'

/* A storage parameter: a table's, of its TOAST table if toast, or a column's. */
typedef struct WsOption
{
	bool		toast;
	char	   *name;
	char	   *value;
} WsOption;

#define WS_STORAGE_DEFAULT '\0'	/* the type's */
#define WS_COMPRESSION_DEFAULT '\0'

typedef struct WsColumn
{
	uint64		tag;
	char	   *name;
	char	   *type;
	char		kind;			/* WS_COL_* */
	char	   *not_null;		/* its not-null constraint's name, or NULL: it may be null */
	char		identity;		/* WS_IDENTITY_* */
	char	   *identity_seq;	/* an identity's sequence's name, in the table's schema */
	char	   *def;			/* default or generation expression, or NULL */
	char	   *collation;		/* qualified and quoted, or NULL: the type's */
	char		storage;		/* TYPSTORAGE_*, only if other than the type's */
	char		compression;	/* TOAST_*_COMPRESSION, or WS_COMPRESSION_DEFAULT */
	bool		has_stattarget;
	int			stattarget;		/* SET STATISTICS, if it has one */
	int			noptions;
	WsOption   *options;		/* by name in UTF8's order */
} WsColumn;

#define WS_INDEX_ORDER_DEFAULT 'd'
#define WS_INDEX_ORDER_DESC 'D'
/* (the default's the other: first if descending, else last) */
#define WS_INDEX_NULLS_DEFAULT 'd'
#define WS_INDEX_NULLS_FIRST 'f'
#define WS_INDEX_NULLS_LAST 'l'

/* One of an index's keys: a column's tag, or an expression (tag 0). */
typedef struct WsIndexKey
{
	uint64		tag;
	char	   *expr;			/* as stored, or NULL: a column */
	char	   *collation;		/* quoted, qualified if not visible, or NULL: the default */
	char	   *opclass;		/* quoted, qualified if not visible, or NULL: the default */
	char		order;			/* WS_INDEX_ORDER_* */
	char		nulls;			/* WS_INDEX_NULLS_* */
	char	   *op;				/* an exclusion's operator, qualified if not visible, or NULL */
} WsIndexKey;

/*
 * An index no constraint has: a statement of its own, after the table's.  An
 * exclusion constraint's too, with no name but the constraint's, and each key
 * with its operator.
 */
typedef struct WsIndex
{
	char	   *name;
	char	   *method;			/* its access method's name */
	bool		unique;
	bool		nulls_not_distinct;
	int			nkeys;
	WsIndexKey *keys;
	int			ninclude;
	uint64	   *include;		/* INCLUDE's tags */
	char	   *predicate;		/* as stored, or NULL: not partial */
} WsIndex;

#define WS_CONSTRAINT_CHECK 'c'
#define WS_CONSTRAINT_UNIQUE 'u'
#define WS_CONSTRAINT_EXCLUSION 'x'

typedef struct WsConstraint
{
	char	   *name;
	char		kind;			/* WS_CONSTRAINT_* */
	char	   *expr;			/* a check's expression, as stored */
	int			ncols;
	uint64	   *cols;			/* a unique constraint's tags */
	bool		nulls_not_distinct;
	bool		deferrable;
	bool		initdeferred;
	bool		not_valid;		/* a check's: a statement of its own, after the rows */
	bool		no_inherit;		/* a check's */
	bool		not_enforced;	/* a check's; which says it is not valid, too */
	WsIndex		excl;			/* an exclusion's index */
} WsConstraint;

/* A statistics target on an index's expression column, numbered from one. */
typedef struct WsIndexStat
{
	int			column;
	int			target;
} WsIndexStat;

/* The operator class parameters on one of an index's keys, numbered from one. */
typedef struct WsKeyOptions
{
	int			column;
	int			noptions;
	WsOption   *options;		/* by name in UTF8's order */
} WsKeyOptions;

/*
 * What an index has beyond its definition, by its name: a primary key's, a unique
 * or exclusion constraint's, or one of its own.  Only one that has any is listed.
 */
typedef struct WsIndexSettings
{
	char	   *index;
	int			ninclude;
	uint64	   *include;		/* INCLUDE's tags, of a key or a unique constraint */
	int			noptions;
	WsOption   *options;		/* by name in UTF8's order */
	int			nstats;
	WsIndexStat *stats;			/* by column */
	int			nkeyoptions;
	WsKeyOptions *keyoptions;	/* by column */
} WsIndexSettings;

#define WS_REPLICA_DEFAULT '\0'
#define WS_REPLICA_FULL 'f'
#define WS_REPLICA_NOTHING 'n'
#define WS_REPLICA_INDEX 'i'

/* The tables one table's record can say it inherits from. */
#define WS_MAX_PARENTS 1600

#define WS_PARTITION_NONE '\0'
#define WS_PARTITION_RANGE 'r'
#define WS_PARTITION_LIST 'l'
#define WS_PARTITION_HASH 'h'

typedef struct WsTableSchema
{
	char	   *name;
	int			ncols;
	WsColumn   *cols;			/* attribute order */
	char	   *key_name;		/* the primary key's constraint, or NULL: none */
	int			nkey;
	uint64	   *key;			/* the primary key's tags */
	int			nconstraints;
	WsConstraint *constraints;	/* other than the key, by name in UTF8's order */
	int			nindexes;
	WsIndex    *indexes;		/* those no constraint has, by name in UTF8's order */
	char		partition;		/* WS_PARTITION_*: how its partitions divide its rows */
	int			npartition;
	WsIndexKey *partition_keys; /* a column or expression, its collation and opclass */
	char	   *partition_of;	/* the partitioned table it is one of, in its schema, or NULL */
	char	   *partition_bound;	/* its FOR VALUES or DEFAULT, of constants, as the server prints it */
	int			ninherits;
	char	  **inherits;		/* the tables it inherits from, in its schema, in order */
	int			ninherited_cols;
	uint64	   *inherited_cols; /* tags of those columns it has only by inheriting, in attribute order */
	int			ninherited_cons;
	char	  **inherited_cons; /* names of those constraints likewise, in UTF8's order */
	int			noptions;
	WsOption   *options;		/* the table's, then its TOAST table's, each by name in UTF8's order */
	bool		key_deferrable;
	bool		key_initdeferred;
	int			nindex_settings;
	WsIndexSettings *index_settings;	/* by index name in UTF8's order */
	char		replica_identity;	/* WS_REPLICA_* */
	char	   *replica_index;	/* WS_REPLICA_INDEX's */
	char	   *clustered;		/* the index CLUSTER uses, or NULL */
	bool		row_security;	/* ENABLE ROW LEVEL SECURITY */
	bool		force_row_security; /* FORCE ROW LEVEL SECURITY */
} WsTableSchema;

/*
 * What a schema must be, in order to be written and when it is read: named,
 * in names Postgres would not shorten, its columns' tags and names distinct,
 * a generated column with its expression and no identity, an identity not
 * null and with its sequence, no name another's of its kind (a not-null
 * constraint's among the table's constraints, a sequence's among its relations),
 * no empty expression or collation, an expression's references well
 * formed, a key named if it has columns, its columns its own, stored, not
 * null and distinct; other constraints in order by name, none named as another
 * or as the key, a check with its expression and flags, not valid or not
 * enforced and not both, a unique one with its columns, its own, stored and
 * distinct, an exclusion with an unnamed
 * index and each key an operator, none alike another, initially deferred only
 * if deferrable; indexes in order by name, none named as a constraint with an
 * index or the key, with a method and keys, each a column of its own or an
 * expression, not both, and no operator, INCLUDE's columns its own, an order of
 * nulls only if it is not the default, and NULLS NOT DISTINCT only if unique;
 * a known way to partition with keys, one if by list, no more than a partition
 * key can have otherwise, none if not partitioned, each key as an index's with no
 * order or operator; a partition with its bound, of another table and with no
 * identity; a table inheriting from no more than WS_MAX_PARENTS others, distinct
 * and not itself, neither a partition nor partitioned, what it has only by doing so its own columns in attribute order and
 * its own checks' and not-null constraints' names in order; storage parameters
 * named, valued and in order, a column's storage, compression and statistics
 * target known ones; an index's settings each for an index it has, in order, its
 * INCLUDE only on a key or unique constraint, its columns' parameters only on a key
 * with an operator class; a replica identity known, naming an index only by index,
 * and a clustered index one it has.  NULL if it is so, or what is wrong.
 */
extern const char *ws_schema_problem(const WsTableSchema *s);

/*
 * The schema codec.  The same schema is always the same bytes, and only those
 * bytes are read as it, on any cluster: a schema's strings are UTF8. encode
 * raises an error if the schema has a problem; decode raises
 * ERRCODE_DATA_CORRUPTED if the bytes are not a schema's, or the schema they
 * hold has one, or a string is not UTF8.  What decode gives is the caller's to
 * ws_schema_free.
 */
typedef struct WsSchemaCodecRoutine
{
	const char *name;
	void		(*encode) (const WsTableSchema *s, Buf *out);
	void		(*decode) (const uint8 *p, size_t len, WsTableSchema *s);
} WsSchemaCodecRoutine;

extern PGDLLIMPORT const WsSchemaCodecRoutine BinaryWsSchemaCodec;

/* The codec the working-set model uses. */
extern PGDLLIMPORT const WsSchemaCodecRoutine *WsSchemaCodec;

extern void ws_schema_free(WsTableSchema *s);

/* A column by tag, or by name, or NULL. */
extern const WsColumn *ws_schema_column(const WsTableSchema *s, uint64 tag);
extern const WsColumn *ws_schema_column_named(const WsTableSchema *s, const char *name);

/*
 * The row codec's shape of the table: the key's columns, then the other
 * stored columns (generated or not) in attribute order, which is COPY's
 * order.  A virtual column has no value.  A schema with a problem (one a merge
 * made, not yet checked) raises an error.
 */
extern void ws_schema_row_shape(const WsTableSchema *s, WsRowShape *shape);

/*
 * The schema of a table, from the catalogs, through WsSql (its foreign keys,
 * which no one record holds, from ws_fk_read, though one no commit can hold
 * refuses the table here too): false if the schema
 * has no ordinary table of that name.  Each column's tag is its catalog row's,
 * so a table outside a versioned schema raises ERRCODE_FEATURE_NOT_SUPPORTED.
 * What the record cannot yet hold and would lose (inheritance or a partition or
 * partitioned table across schemas, a tablespace or another access method, a
 * sequence's, a default using one, a partition detached, of another identity
 * than its table's or with two indexes attaching could take for one another
 * (a parent's constraint takes only a constraint's index: that one is not),
 * a policy, an index's dependence on an extension, comments, grants, two
 * constraints alike)
 * raises ERRCODE_FEATURE_NOT_SUPPORTED, and a name past a commit's limit
 * ERRCODE_NAME_TOO_LONG.  A partition's identity is its table's, and not held
 * in its own record.  The table's owner is not held, and is whoever makes
 * it.  Triggers, rules and the like are objects of their own, not their table's.
 *
 * It locks the table till the transaction ends, and a column renamed, retyped or
 * dropped since the transaction's snapshot raises
 * ERRCODE_T_R_SERIALIZATION_FAILURE.  Other objects' names (a type's, a
 * function's in a default) are printed as they are now, so a reader on an old
 * snapshot must hold those still itself.  An index dropped since that snapshot
 * raises ERRCODE_T_R_SERIALIZATION_FAILURE too.  A sequence a default names only by
 * text has no dependency to see, so the record holds that default as it is.
 * What it gives is the caller's to ws_schema_free.
 */
extern bool ws_schema_read(const char *schema, const char *table, WsTableSchema *out);

/*
 * Gives the columns of a table just made from a record the tags the record
 * says, in place of those they were given as new.  The caller makes them
 * visible, with CommandCounterIncrement.
 */
extern void ws_schema_set_tags(const char *schema, const WsTableSchema *s);

/*
 * The CREATE TABLE statement for the table, in the named schema, appended to
 * out, in the server's encoding: text the encoding cannot hold raises an
 * error, as does a name it holds in too many bytes; each expression's
 * references the names of its table's columns.  Names are quoted as
 * quote_identifier does, under the session's settings: a scope
 * (prolly/ws/ws_sql.h) makes the text the same whoever asks.  A commit may
 * have come from anywhere: the whole statement is parsed and compared with the
 * record, and if it is anything but one CREATE TABLE of just these columns,
 * types, names, clauses, expressions and key, and each constraint but a check not
 * valid (ws_schema_add_check), it raises ERRCODE_DATA_CORRUPTED and is
 * not given back.  A schema with a problem raises an error.  What an
 * expression does is not checked: a default runs as it is, on later inserts,
 * with the inserter's privileges.
 */
extern void ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out);

/*
 * The ALTER TABLE ONLY giving the table's columns their statistics targets and
 * storage parameters, which CREATE TABLE has no clause for, in the named schema,
 * appended to out, to run right after the table's own statement; nothing, if none
 * has any.  Checked as that statement is.
 */
extern void ws_schema_column_settings(const WsTableSchema *s, const char *schema, Buf *out);

/*
 * The CREATE INDEX statement for the table's nth index, in the named schema,
 * appended to out, to run after the table's: checked as the table's is, one
 * statement of just this index on just this table, or it raises
 * ERRCODE_DATA_CORRUPTED and is not given back.
 */
extern void ws_schema_create_index(const WsTableSchema *s, int n, const char *schema, Buf *out);

/*
 * The ALTER TABLE adding the table's nth constraint, a check not valid, in the
 * named schema, appended to out, to run after the table's rows: on any earlier,
 * the rows it lets stand would be refused.  A partition's are added before its
 * table's, which then finds them, and is refused otherwise as one already there;
 * and all before any table inherits, which needs them there.
 * Checked as the table's statement is.
 */
extern void ws_schema_add_check(const WsTableSchema *s, int n, const char *schema, Buf *out);

/*
 * The ALTER TABLE attaching a partition to its partitioned table, in the named
 * schema, appended to out, to run once both are made, with their indexes, and
 * after that table is attached to its own, if it is a partition too: attaching
 * gives a partition its table's identity, and does not pass it further down.  The
 * index of the partition's alike one of the table's becomes its partition, under
 * the name it has.  Checked as the table's statement is, and a bound must hold
 * nothing but constants.
 */
extern void ws_schema_attach(const WsTableSchema *s, const char *schema, Buf *out);

/*
 * The ALTER TABLE ONLY giving a table its replica identity and clustered index, in
 * the named schema, appended to out, to run after its indexes; nothing, if it has
 * neither.  Checked as the table's statement is.
 */
extern void ws_schema_table_settings(const WsTableSchema *s, const char *schema, Buf *out);

/*
 * The ALTER INDEX giving the nth of a table's index_settings its statistics
 * targets, in the named schema, appended to out, to run after its indexes; nothing,
 * if it has none.  Checked as the table's statement is.
 */
extern void ws_schema_index_stats(const WsTableSchema *s, int n, const char *schema, Buf *out);

/*
 * The ALTER TABLE ONLY enabling and forcing a table's row security, in the named
 * schema, appended to out, to run last, after its rows and every foreign key on it
 * or to it: with it forced, a role that does not bypass it would have those rows
 * refused, and the query checking a new key's rows is subject to it, unlike later
 * ones.  Nothing, if it has neither.  Checked as the table's statement is.
 */
extern void ws_schema_row_security(const WsTableSchema *s, const char *schema, Buf *out);

/*
 * The ALTER TABLE making a table inherit from its nth parent, in the named schema,
 * appended to out, to run in order, once both are made, with their rows and checks
 * not valid: a parent's check not enforced needs the child's of that name, not
 * valid, already there.
 * Checked as the table's statement is.
 */
extern void ws_schema_inherit(const WsTableSchema *s, int n, const char *schema, Buf *out);

/*
 * Marks what a table inheriting has only by inheriting as not its own, as making
 * it from its own statement could not: dropping it from a parent then drops it
 * here too.  To run after it has inherited from its parents.  What
 * the record says that it does not inherit raises ERRCODE_DATA_CORRUPTED.  The
 * caller makes it visible, with CommandCounterIncrement.
 */
extern void ws_schema_set_inherited(const char *schema, const WsTableSchema *s);

/*
 * A name as a commit holds it, raising ERRCODE_NAME_TOO_LONG past a commit's
 * limit; and as this database holds it, raising the same if it holds it in too
 * many bytes, where the grammar would cut it short.  NULL gives NULL, there.
 */
extern char *ws_name_to_utf8(const char *name);
extern char *ws_name_from_utf8(const char *name);

/* Text of a statement being written; an identifier as quote_identifier quotes it. */
extern void ws_put_text(Buf *out, const char *text);
extern void ws_put_identifier(Buf *out, const char *name);

#endif							/* PROLLY_WS_SCHEMA_H */
