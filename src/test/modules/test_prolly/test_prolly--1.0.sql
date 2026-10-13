/* src/test/modules/test_prolly/test_prolly--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_prolly" to load this file. \quit

-- Every registered implementation of every prolly interface.
CREATE FUNCTION prolly_implementations(
	OUT interface text,
	OUT name text,
	OUT impl_id int4,
	OUT format_version int4,
	OUT description text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Run the conformance suite of each interface against its implementations.
-- NULL arguments mean all interfaces / all implementations.
CREATE FUNCTION prolly_conformance(
	only_interface text DEFAULT NULL,
	only_implementation text DEFAULT NULL,
	OUT interface text,
	OUT implementation text,
	OUT test text,
	OUT result text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Write n nodes to a node store implementation, with hashes and contents
-- derived from seed, so prolly_store_verify can find them later (for
-- example after crash recovery).  Returns the number written.
CREATE FUNCTION prolly_store_write(store text, seed int8, n int4)
RETURNS int4
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Check that the n nodes written by prolly_store_write(store, seed, n) are
-- all found by hash and have the right contents.  Returns n, or raises an
-- error.
CREATE FUNCTION prolly_store_verify(store text, seed int8, n int4)
RETURNS int4
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
