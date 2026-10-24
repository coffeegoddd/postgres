/*-------------------------------------------------------------------------
 *
 * prolly_extconfig.h
 *	  Extensions' configuration rows, as commits record them.
 *
 * An extension's configuration tables (pg_extension.extconfig, such as
 * PostGIS's spatial_ref_sys) are shared by every branch and aren't
 * versioned, but the commits' data may refer to their rows.  So an
 * extension's dependency records what pg_dump dumps with it: each row its
 * extcondition selects, and each configuration sequence's state, one
 * element each.  A row is recorded column by column, by name, each value
 * in its canonical form (prolly_datum.h), so the element is the same
 * whatever the session's settings, and a table whose columns are ordered
 * otherwise reads it back.  A clone writes the elements into the extension
 * it makes, as a restore of a dump does; a move checks the database has
 * every recorded row.
 *
 * src/include/access/prolly_extconfig.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_EXTCONFIG_H
#define PROLLY_EXTCONFIG_H

#include "lib/stringinfo.h"
#include "nodes/pg_list.h"

/* The prefixes of a dependency's configuration elements. */
#define PROLLY_EXTCONFIG_ROW "config\t"
#define PROLLY_EXTCONFIG_SEQUENCE "configseq\t"

/* Whether a dependency's element is a configuration element. */
extern bool prolly_extconfig_element(const char *element);

/*
 * An extension's configuration elements (char *, sorted), read as each
 * table's owner with row security off, as pg_dump reads them; *has_config
 * is set if it has configuration tables or sequences at all.
 */
extern List *prolly_extconfig_elements(Oid extoid, bool *has_config);

/*
 * A digest of what prolly_extconfig_elements() would give for extoid, much
 * cheaper than it: equal digests mean equal elements.
 */
extern char *prolly_extconfig_fingerprint(Oid extoid);

/*
 * Check elements (an extension's, by name) against the database: with
 * restore (an extension a clone just made), write the rows the
 * configuration tables lack and set the sequences, as a restore of a dump
 * would; without, append to missing what rows the tables lack.  An element
 * naming a table that isn't one of the extension's configuration tables,
 * or a column it lacks, is an error.  Where elements hold one key with
 * different values, a restore writes preferred's (sorted; the head a clone
 * checks out), warning of the others.
 */
extern void prolly_extconfig_apply(const char *extname, List *elements,
								   List *preferred, bool restore,
								   StringInfo missing);

#endif							/* PROLLY_EXTCONFIG_H */
