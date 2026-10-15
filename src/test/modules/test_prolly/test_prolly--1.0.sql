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
-- rel is a table the transaction hasn't written, empty or not.
CREATE FUNCTION prolly_seek_sees_later_writes(rel regclass)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Check that catalog rows formed with a null branch column don't leak the
-- rows written in their place.  Inserts into pg_description; roll back.
CREATE FUNCTION test_catalog_null_branch_no_leak(n int)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Make the branch called name the session's branch.
CREATE FUNCTION test_branch_switch(name text)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The session's branch.
CREATE FUNCTION test_branch_current()
RETURNS oid
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Block-mapped storage's sharing, below the catalogs: storage is named by
-- relation file number in the database's default tablespace.  A copy
-- made by test_bmap_share() lasts until test_bmap_unlink() drops it, or,
-- if transactional, until the transaction aborts.
CREATE FUNCTION test_bmap_share(relnumber oid, transactional bool DEFAULT false)
RETURNS oid
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_digest(relnumber oid)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_map(relnumber oid,
	OUT blkno int8, OUT phys int8, OUT epoch int8, OUT refs int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_epoch(relnumber oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_free_blocks()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_unlink(relnumber oid)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_truncate(relnumber oid, nblocks int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_extend(relnumber oid, n int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Change a byte of a page's hole, as an access method changes a page.
CREATE FUNCTION test_bmap_write(relnumber oid, blkno int, value int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A digest of heap pages' rows, leaving out hint bits.
CREATE FUNCTION test_bmap_rows_digest(relnumber oid)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Read a block zeroed and locked, as an access method about to initialize
-- it does (only if it isn't in a buffer), and leave it uninitialized.
CREATE FUNCTION test_bmap_zero_block(relnumber oid, blkno int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Try for a block's cleanup lock without waiting; true if it was free.
CREATE FUNCTION test_bmap_cleanup_lock(relnumber oid, blkno int)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
