/*-------------------------------------------------------------------------
 *
 * ws_tags.h
 *	  Column references by tag in the working-set model's stored text: an
 *	  expression or a definition follows a column another branch renamed.
 *
 * In stored text @{<tag>} refers to a column, and @@ is an @.  Every @ of the
 * text as written is doubled, so nothing in it (a string, an operator) is
 * taken for a reference; a lone @ is corruption.  Rendering gives the text
 * back, each reference the name its column has in the commit being read.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_tags.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_TAGS_H
#define PROLLY_WS_TAGS_H

#include "access/tupdesc.h"
#include "prolly/engine/util.h"
#include "utils/relcache.h"

/* A column's name by tag (as it is, not quoted), or NULL if it has none. */
typedef const char *(*WsTagNameFn) (void *arg, uint64 tag);

/* A column's tag by name; false if it has no such column. */
typedef bool (*WsTagOfFn) (void *arg, const char *name, uint64 *tag);

/*
 * Whether stored text is well formed, each reference a tag in its one form:
 * the same text is always the same bytes.
 */
extern bool ws_tags_well_formed(const char *stored);

/*
 * A tag from a table's name, a column's name and its type (their UTF8), so
 * that two branches adding the same column agree on it, in every cluster.
 * attempt is 0, or more if the table has another column of that tag.
 */
extern uint64 ws_tag_derive(const char *table, const char *column, const char *type,
							int attempt);

/*
 * The tag of a column being added to a table, none of the table's others: 0
 * if the table is not an ordinary or partitioned one in a versioned schema.
 * A table cannot move out of or into versioned schemas, so that is for life.
 * Text the name is in that has no UTF8 raises an error: no commit could hold it.
 */
extern uint64 ws_tag_new_column(Relation rel, const FormData_pg_attribute *att);

/* The tags of a new relation's columns (a descriptor copied holds its source's). */
extern void ws_tags_assign(Relation rel);

/*
 * An expression as the catalogs print it, as stored text appended to out:
 * each reference to a column of the table by tag, found by the grammar (no
 * string or comment holds one), and every other @ doubled.  A column it
 * refers to that tag_of has not raises an error.
 */
extern void ws_tags_template(const char *expression, WsTagOfFn tag_of, void *arg, Buf *out);

/* Text as written, appended to out as stored: with no references. */
extern void ws_tags_escape(const char *text, Buf *out);

/*
 * Stored text appended to out as written, each reference its column's name,
 * quoted.  False if a column has no name, *missing its tag.  Malformed text
 * raises ERRCODE_DATA_CORRUPTED.
 */
extern bool ws_tags_render(const char *stored, WsTagNameFn name, void *arg, Buf *out,
						   uint64 *missing);

#endif							/* PROLLY_WS_TAGS_H */
