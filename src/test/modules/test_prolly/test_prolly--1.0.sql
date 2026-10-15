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

-- The canonical bytes of a value (prolly_datum.h), and the value decoded
-- back from them.
CREATE FUNCTION prolly_canonical_value(anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION prolly_canonical_round_trip(anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many branch heads and roots waiting to be published or abandoned a
-- commit pipeline implementation holds, across all databases.
CREATE FUNCTION prolly_commit_usage(implementation text,
	OUT heads int4, OUT pending int4)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Make this backend's next transaction fail just before it commits.
CREATE FUNCTION prolly_fail_next_commit()
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Commit a made-up root to a branch (after the main one) of the current
-- database through a commit pipeline, as part of the current transaction.
CREATE FUNCTION prolly_commit_test_enqueue(implementation text, branch int8)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The number of items in a prolly relation's tree and a digest of them.
CREATE FUNCTION prolly_tree_digest(rel regclass)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Check that seeks of rel's cursors see writes made after they opened;
-- rel is an empty table the transaction hasn't written.
CREATE FUNCTION prolly_seek_sees_later_writes(rel regclass)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
