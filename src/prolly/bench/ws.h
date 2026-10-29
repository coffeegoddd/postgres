/*
 * ws.h
 *	  The working set harness's pieces: the server, the working set's
 *	  capture and restore (logical, and from the replication stream), and
 *	  what the checks share with them.
 */
#ifndef WS_H
#define WS_H

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <ctype.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../test/test_util.h"
#include "pgoutput.h"

#ifndef PGBIN
#define PGBIN "/home/dustin/cursor_src/pg-v2-build/inst/bin"
#endif

#define SCHEMA "rt"
#define TEMPLATE_DB "rt_template"	/* the extensions, copied into every database */
#define WS_MAX_COLS 1600		/* Postgres's limit */

/* Settings under which row text is written and read. */
#define SESSION_SETTINGS \
	"SET DateStyle = ISO; SET IntervalStyle = postgres; SET TimeZone = 'UTC'; " \
	"SET extra_float_digits = 1; SET bytea_output = hex; SET search_path = ''"

typedef struct Opts
{
	const char *conninfo;
	const char *fixture;
	const char *work;
	const char *bindir;
	const char *src_db;
	const char *dst_db;
	const char *brk;			/* a deliberate fault, to show the checks catch it */
	int			steps;			/* 0: all */
	bool		stream;			/* commits from the replication stream */
	const char *sqldir;			/* prolly_tags.sql and prolly_triggers.sql */
	const char *only;			/* --merge: these scenario numbers (comma separated) */
} Opts;

extern Opts o;
extern bool broken(const char *what);
extern void die(const char *fmt, const char *a);

/* --------------------------------------------------------------- server */

extern PGconn *pg_open(const char *db);
extern PGresult *pg_exec(PGconn *c, const char *sql);
extern void pg_cmd(PGconn *c, const char *sql);
extern bool run(const char *cmd);
extern void psql_file(const char *db, const char *file);
extern void fresh_database(const char *db);
extern char *slurp_cmd(const char *cmd, size_t *len);
extern char *slurp_file(const char *path, size_t *len);
extern void write_file(const char *path, const void *data, size_t len);
extern char *dump_section(const char *db, const char *section, size_t *len);
extern char dump_snapshot[64];

/* ---------------------------------------------------------------- codec */

typedef struct WsCol
{
	uint64_t	tag;
	char		kind;			/* s stored, g generated */
	int			pos;			/* among stored columns by attribute; -1 if generated */
	char	   *type;
	char	   *name;			/* quoted */
	char	   *def;			/* default or generation expression, or empty */
} WsCol;

typedef struct WsLayout
{
	char	   *rel;			/* quoted */
	int			nkey;
	uint64_t   *key;			/* primary key tags, in key order */
	int			ncols;
	WsCol	   *cols;			/* attribute order */
	int			nstored;
	uint64_t   *copy;			/* stored tags in COPY order: key, then the rest */
	int		   *stream;			/* stream column -> COPY index */
} WsLayout;

/* A stored column's value: its output text, or NULL. */
typedef struct WsField
{
	const uint8_t *p;
	size_t		len;
	bool		null;
} WsField;

typedef struct WsCell
{
	uint64_t	tag;
	const uint8_t *p;
	size_t		len;
} WsCell;

extern bool ws_layout_parse(const char *text, size_t len, WsLayout *L);
extern void ws_layout_free(WsLayout *L);
extern void ws_layout_write(const WsLayout *L, Buf *out);
extern const WsCol *ws_layout_col(const WsLayout *L, uint64_t tag);
extern const WsCol *ws_layout_col_named(const WsLayout *L, const char *quoted);
extern int	ws_copy_index(const WsLayout *L, uint64_t tag);
extern void ws_copy_list(const WsLayout *L, Buf *out);
extern void ws_cells_encode(WsCell *c, int n, Buf *out);
extern int	ws_cells(const uint8_t *v, size_t vl, WsCell *out, int max);
extern bool ws_cell_get(const uint8_t *v, size_t vl, uint64_t tag, const uint8_t **p,
						size_t *len);
extern void ws_encode(const WsLayout *L, const WsField *f, Buf *key, Buf *val);
extern void ws_decode(const WsLayout *L, const uint8_t *k, size_t kl, const uint8_t *v,
					  size_t vl, WsField *f);
extern void ws_copy_escape(Buf *b, const uint8_t *v, size_t len);
extern void ws_copy_split(const char *line, size_t len, WsField *f, int n, Buf *scratch);
extern void ws_copy_line(const WsLayout *L, const WsField *f, Buf *line);

/* ---------------------------------------------------------- working set */

/*
 * How a schema's working set becomes a root value and back.  capture reads
 * the schema in a database into a root value; restore builds it, from the
 * root value alone, into a database holding only the extensions and
 * prolly_tags.sql.
 */
typedef struct WorkingSetOps
{
	const char *name;
	PHash		(*capture) (ProllyCtx *ctx, const char *db);
	void		(*restore) (ProllyCtx *ctx, const PHash *rv, const char *db);
} WorkingSetOps;

/* What did not restore: an object that never succeeded, or a table's rows. */
typedef struct WsProblem
{
	char	   *what;			/* "<type> <name>" or "rows <table>" */
	char	   *sqlstate;
	char	   *constraint;
	char	   *column;
	char	   *message;
	char	   *key;			/* the object's key (columns by tag), or empty */
	char	   *ddl;			/* the definition that failed, or empty */
} WsProblem;

typedef struct WsReport
{
	WsProblem  *p;
	size_t		n,
				cap;
} WsReport;

typedef struct Row
{
	uint8_t    *key;
	size_t		klen;
	uint8_t    *val;
	size_t		vlen;
} Row;

typedef struct TableList
{
	char	  **names;
	VcTable    *t;
	size_t		n,
				cap,
				tcap;
} TableList;

/*
 * Every table holding rows in the schema, with its layout (ws_codec.c):
 *	oid, name, persistence, layout, and an SQL expression giving a row's
 *	COPY text in COPY order (for the event trigger).
 * A column's pos is its place among the stored columns by attribute number,
 * which is the order a replication stream sends them in.  The row
 * expression formats each value with its type's output function (as COPY
 * and the stream do), not a cast to text: casts differ for some types
 * (bool, char(n), inet).  The capture and the event trigger both run this
 * query, so they agree on layouts byte for byte.  Defaults are shown in
 * UTC, the session's time zone, so they are the same wherever captured.
 */
#define LAYOUT_SQL \
	"WITH rel AS (SELECT c.oid, c.relname, c.relpersistence FROM pg_class c " \
	"  JOIN pg_namespace n ON n.oid = c.relnamespace " \
	"  WHERE n.nspname = '" SCHEMA "' AND c.relkind = 'r'), " \
	"pk AS (SELECT i.indrelid, k.attnum, k.ord FROM pg_index i, " \
	"  unnest(i.indkey::int2[]) WITH ORDINALITY k(attnum, ord) " \
	"  WHERE i.indisprimary AND k.ord <= i.indnkeyatts), " \
	"col AS (SELECT r.oid, a.attname, a.attnum, a.attgenerated, p.ord, g.tag, " \
	"    format_type(a.atttypid, a.atttypmod) AS typ, pg_get_expr(d.adbin, d.adrelid) AS def, " \
	"    CASE WHEN a.attgenerated = '' THEN " \
	"      rank() OVER (PARTITION BY r.oid, a.attgenerated = '' ORDER BY a.attnum) - 1 " \
	"    ELSE -1 END AS pos FROM rel r " \
	"  JOIN pg_attribute a ON a.attrelid = r.oid AND a.attnum > 0 AND NOT a.attisdropped " \
	"  LEFT JOIN pk p ON p.indrelid = r.oid AND p.attnum = a.attnum " \
	"  LEFT JOIN prolly.tags g ON g.relid = r.oid AND g.attnum = a.attnum " \
	"  LEFT JOIN pg_attrdef d ON d.adrelid = r.oid AND d.adnum = a.attnum), " \
	"lay AS (SELECT r.oid, r.relname, r.relpersistence, " \
	"  (SELECT string_agg(tag::text, ',' ORDER BY ord) FROM col " \
	"     WHERE col.oid = r.oid AND ord IS NOT NULL) AS keytags, " \
	"  (SELECT string_agg(format(E'col %s\\t%s\\t%s\\t%s\\t%s\\t%s\\n', tag, " \
	"       CASE WHEN attgenerated = '' THEN 's' ELSE 'g' END, pos, typ, " \
	"       quote_ident(attname), coalesce(def, '')), '' ORDER BY attnum) " \
	"     FROM col WHERE col.oid = r.oid) AS cols, " \
	"  (SELECT string_agg(format('prolly.esc(num_nulls(%I) > 0, format(''%%s'', %I))', attname, attname), " \
	"     $$ || E'\\t' || $$ ORDER BY ord NULLS LAST, attnum) " \
	"     FROM col WHERE col.oid = r.oid AND attgenerated = '') AS rowexpr " \
	"  FROM rel r) " \
	"SELECT oid, relname, relpersistence, " \
	"  format(E'rel %s\\nkey %s\\n%s', quote_ident(relname), coalesce(keytags, ''), " \
	"    coalesce(cols, '')) AS layout, rowexpr " \
	"FROM lay ORDER BY relname COLLATE \"C\""

extern const WorkingSetOps logical_ops;
extern const WorkingSetOps stream_ops;

extern void row_from_line(const WsLayout *L, const char *line, size_t len, Row *r);
extern void row_add(Row **rows, size_t *n, size_t *cap, const WsLayout *L, const char *line,
					size_t len);
extern PHash rows_build(ProllyCtx *ctx, Row *rows, size_t n, const WsLayout *L);
extern PHash capture_rows(ProllyCtx *ctx, PGconn *c, const WsLayout *L);
extern PHash capture_sequences(ProllyCtx *ctx, PGconn *c);
extern PHash put_table(ProllyCtx *ctx, PHash rv, const char *name, const char *schema,
					   size_t slen, const PHash *data);
extern PHash capture_defs(ProllyCtx *ctx, PHash rv, PGconn *c, const char *db);
extern void layout_must_parse(const char *text, WsLayout *L);
extern PHash logical_capture(ProllyCtx *ctx, const char *db);
extern void table_list(ProllyCtx *ctx, const PHash *rv, TableList *l);
extern void table_list_free(TableList *l);
extern char *schema_text(ProllyCtx *ctx, const PHash *h, size_t *len);
extern bool table_layout(ProllyCtx *ctx, const PHash *rv, const char *name, WsLayout *L,
						 PHash *data);
extern void report_add(WsReport *rep, const char *what, const PGresult *res);
extern void report_add_ddl(WsReport *rep, const char *what, const PGresult *res,
						   const char *key, const char *ddl);
extern void report_free(WsReport *rep);
extern void ws_restore(ProllyCtx *ctx, const PHash *rv, const char *db, WsReport *rep);
extern void ws_install(const char *db, bool triggers);
extern void ws_install_triggers(const char *db);

/* --------------------------------------------------------------- stream */

#define STREAM_SLOT "prolly_rt"
#define STREAM_PUB "prolly_rt"
#define DDL_LOCK "4242"
#define PEND_MAX 200000			/* pending edits applied beyond this */
#define BARRIER_DEADLINE 5.0	/* seconds; the fixture's steps take well under 1 */

typedef struct PendEnt
{
	size_t		koff,
				klen,
				voff,
				vlen;
	bool		del;
} PendEnt;

typedef struct STable
{
	uint32_t	oid;
	char	   *name;
	char	   *layout;
	char		persist;
	WsLayout	L;
	PHash		data;
	bool		live;
	/* Edits not yet applied to data; the last edit of a key wins. */
	Buf			arena;
	PendEnt    *pe;
	size_t		npe,
				pecap;
	size_t	   *slot;			/* pe index + 1, by key hash */
	size_t		scap;
} STable;

typedef struct Stream
{
	ProllyCtx  *ctx;
	PgoStream  *pgo;
	STable	   *t;
	size_t		n,
				cap;
	char		token[32];
	bool		stop;
	uint64_t	barrier_lsn;
	bool		unflushed;		/* the last barrier was not flushed when written */
	uint64_t	changes,
				ddl_msgs,
				rows_msgs,
				rows_reemitted,
				toast_filled;
	double		wait_s,			/* reading the stream to the barrier */
				defs_s;			/* definitions and sequences at the snapshot */
} Stream;

extern Stream stream;

extern void drop_slot(void);
extern void stream_setup(ProllyCtx *ctx, const char *db);
extern PHash stream_capture(ProllyCtx *ctx, const char *db);
extern void st_free(STable *t);

/* ------------------------------------------------------ tags in objects */

typedef struct ColRef
{
	char	   *table;			/* quoted */
	char	   *name;			/* quoted */
	uint64_t	tag;
} ColRef;

typedef struct ColMap
{
	ColRef	   *c;
	size_t		n,
				cap;
} ColMap;

extern void colmap_from_db(PGconn *c, ColMap *m);
extern void colmap_from_tree(ProllyCtx *ctx, const PHash *rv, ColMap *m);
extern PHash colmap_build(ProllyCtx *ctx, const ColMap *m);
extern void colmap_free(ColMap *m);
extern void templ(const char *s, size_t n, const ColMap *m, Buf *out);
extern bool render(const char *s, size_t n, const ColMap *m, Buf *out, uint64_t *missing);

/* ----------------------------------------------------------- violations */

/* "violation <constraint> <table> <rowkey>" ("*" for no row), one each. */
typedef struct WsLines
{
	char	  **line;
	size_t		n,
				cap;
} WsLines;

extern void lines_add(WsLines *l, const char *line);
extern void lines_free(WsLines *l);
extern void ws_record_violations(const char *db, const WsReport *rep, WsLines *out,
								 WsLines *unapplied);
extern int	ws_resolve(const char *db, WsLines *unresolved);
extern PHash capture_violations(ProllyCtx *ctx, PGconn *c);
extern void restore_violations(ProllyCtx *ctx, PGconn *c, const PHash *data);

/* -------------------------------------------------------- diff and merge */

/* What changed between two root values, as lines a test can match. */
typedef struct WsDiff
{
	char	  **line;
	size_t		n,
				cap;
} WsDiff;

extern void ws_diff(ProllyCtx *ctx, const PHash *from, const PHash *to, WsDiff *out);
extern bool ws_diff_has(const WsDiff *d, const char *line);
extern void ws_diff_free(WsDiff *d);

/* Evaluates a column default to its output text, for rows merged without it. */
typedef bool (*WsDefaultFn) (void *arg, const char *expr, const char *type, Buf *out);

typedef struct WsMerge
{
	PHash		root;			/* the merged root value, if no conflicts */
	char	  **conflict;		/* "<kind> <where>" */
	size_t		nconflicts,
				cap;
	uint64_t	cells_merged;	/* rows both sides changed, merged by column */
} WsMerge;

extern void ws_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours,
					 const PHash *theirs, WsDefaultFn def, void *defarg, WsMerge *out);
extern void ws_merge_free(WsMerge *m);

#endif
