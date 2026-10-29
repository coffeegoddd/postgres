/* contrib/prolly_wal/prolly_wal--1.0.sql */

\echo Use "CREATE EXTENSION prolly_wal" to load this file. \quit

CREATE FUNCTION put_chunk(data bytea) RETURNS text
AS 'MODULE_PATHNAME', 'prolly_wal_put_chunk' LANGUAGE C STRICT;

CREATE FUNCTION get_chunk(hash text) RETURNS bytea
AS 'MODULE_PATHNAME', 'prolly_wal_get_chunk' LANGUAGE C STRICT;

CREATE FUNCTION set_root(name text, hash text) RETURNS void
AS 'MODULE_PATHNAME', 'prolly_wal_set_root' LANGUAGE C STRICT;

CREATE FUNCTION get_root(name text) RETURNS text
AS 'MODULE_PATHNAME', 'prolly_wal_get_root' LANGUAGE C STRICT;
