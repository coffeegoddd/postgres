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

-- Folds: begin one (its changes list's lineage, its epoch, the length
-- then); what changed since one began; let go of what changed before one
-- began; a page as a fold kept it.
CREATE FUNCTION test_bmap_fold_begin(relnumber oid, OUT lineage int8,
	OUT epoch int8, OUT nblocks int8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_fold_changes(relnumber oid, lineage int8, epoch int8,
	OUT kind text, OUT blkno int8, OUT old_block int8, OUT made int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_fold_prune(relnumber oid, lineage int8, epoch int8)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_read_old(relnumber oid, blkno int8, old_block int8)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The server's chunk store, from SQL: chunks are named by their SHA-256,
-- in hex.
CREATE FUNCTION test_chunk_put(data bytea)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_chunk_get(hash text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_chunk_has(hash text)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_chunk_root()
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Move the root from expected (NULL: none) to root; false if another
-- transaction moved it since.
CREATE FUNCTION test_chunk_move_root(expected text, root text)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Add a table file of these chunks; rewrite the table files as one; list
-- the table files.
CREATE FUNCTION test_chunk_table_file(chunks bytea[])
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_chunk_conjoin()
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_chunk_table_files(OUT name text, OUT chunks int,
	OUT bytes bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C;

-- The canonical root of the session branch's versioned schemas, folded
-- from their tables' rows: from the branch's baseline, as commits fold, or
-- (full) from every row.
CREATE FUNCTION test_fold_root(full bool DEFAULT false)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Whether the refs' walk, what reclamation keeps, reaches a commit.
CREATE FUNCTION test_refs_walk_reaches(commit_id text)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The value of the refs' entry key (a tag, then a name).
CREATE FUNCTION test_refs_value(key text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many nodes the refs' walk visits, one node (by hash) declined.
CREATE FUNCTION test_refs_walk_count(declined text)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Each table's data tree in the session branch's working set, folded
-- once: its qualified name, the tree's hash, and why its tree can't be
-- read as ordered now, if it can't.
CREATE FUNCTION test_fold_trees(OUT name text, OUT tree text, OUT unreadable text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- What the session's folds have read (pages, candidate rows, tables read
-- in full) since the counters were last reset.
CREATE FUNCTION test_fold_counters(reset bool DEFAULT false, OUT pages int8,
	OUT candidates int8, OUT full_tables int8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The refs, below the version-control functions: a commit of the working
-- set with a parent given (as a merge's commit is made), the refs' epoch,
-- and a head moved with nothing else (as a fast-forward's).
CREATE FUNCTION test_commit_with(parent text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_refs_epoch()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_refs_set_head(ref text)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The folded canonical tree's items, for inspection.
CREATE FUNCTION test_fold_items(OUT key text, OUT value text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C;

-- The row applier: insert new, update old to new, or remove old, in the
-- table whose row type they are.
CREATE FUNCTION test_apply(op text, old anyelement, new anyelement)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Add a chunk to a remote under the hash given, whatever its bytes hash to.
CREATE FUNCTION test_remote_put(url text, hash text, data bytea)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Make a new branch of a remote's at commit, recording format if it has none.
CREATE FUNCTION test_remote_set_head(url text, branch text, commit_id text,
	format text)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A folded root without its "!:format" item, as an older build made it.
CREATE FUNCTION test_root_without_format(root text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A commit with no parents naming root.
CREATE FUNCTION test_commit_of(root text, message text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- n commits in a line after parent, each naming its root; the last one.
CREATE FUNCTION test_commit_chain(parent text, n int)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A branch's upstream: a remote and its branch.
CREATE FUNCTION test_refs_set_upstream(branch text, remote text,
	remote_branch text)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many nodes the refs' walk from one commit visits.
CREATE FUNCTION test_walk_commit_count(commit_id text)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
