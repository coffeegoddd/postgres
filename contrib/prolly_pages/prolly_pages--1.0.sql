/* contrib/prolly_pages/prolly_pages--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION prolly_pages" to load this file. \quit

CREATE FUNCTION prolly_put_pages(rel regclass, fork text, blocks int4[], pages bytea)
RETURNS int4
AS 'MODULE_PATHNAME', 'prolly_put_pages'
LANGUAGE C STRICT;

CREATE FUNCTION prolly_put_pages_file(rel regclass, fork text, blocks int4[], path text,
									  batched bool)
RETURNS int4
AS 'MODULE_PATHNAME', 'prolly_put_pages_file'
LANGUAGE C STRICT;

CREATE FUNCTION prolly_get_pages_frozen(rel regclass, fork text, blocks int4[])
RETURNS bytea
AS 'MODULE_PATHNAME', 'prolly_get_pages_frozen'
LANGUAGE C STRICT;

CREATE FUNCTION prolly_apply_insert(rel regclass, cols text[], data text)
RETURNS int8
AS 'MODULE_PATHNAME', 'prolly_apply_insert'
LANGUAGE C STRICT;

CREATE FUNCTION prolly_apply_delete(rel regclass, cols text[], data text)
RETURNS int8
AS 'MODULE_PATHNAME', 'prolly_apply_delete'
LANGUAGE C STRICT;

CREATE FUNCTION prolly_truncate(rel regclass, nblocks int4)
RETURNS void
AS 'MODULE_PATHNAME', 'prolly_truncate'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION prolly_put_pages(regclass, text, int4[], bytea) FROM PUBLIC;
REVOKE ALL ON FUNCTION prolly_put_pages_file(regclass, text, int4[], text, bool) FROM PUBLIC;
REVOKE ALL ON FUNCTION prolly_get_pages_frozen(regclass, text, int4[]) FROM PUBLIC;
REVOKE ALL ON FUNCTION prolly_apply_insert(regclass, text[], text) FROM PUBLIC;
REVOKE ALL ON FUNCTION prolly_apply_delete(regclass, text[], text) FROM PUBLIC;
REVOKE ALL ON FUNCTION prolly_truncate(regclass, int4) FROM PUBLIC;
