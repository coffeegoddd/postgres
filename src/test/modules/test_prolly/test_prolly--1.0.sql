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

-- The canonical bytes of a value (prolly_datum.h), and the value decoded
-- back from them.
CREATE FUNCTION prolly_canonical_value(anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION prolly_canonical_round_trip(anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The value of the given type whose canonical bytes are these.
CREATE FUNCTION prolly_canonical_decode(bytea, anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME' LANGUAGE C;

-- A value stored in the chunk store, as the hash pointer that names it;
-- and a hash pointer's form, raw and stored sizes and root (NULL if the
-- value isn't one).
CREATE FUNCTION test_toast_hash_pointer(anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_toast_pointer(anyelement,
	OUT form int, OUT rawsize int, OUT stored int, OUT root text)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The value tree nodes this backend read and wrote for values stored by
-- content.
CREATE FUNCTION test_toast_counters(reset bool DEFAULT false,
	OUT nodes_read int8, OUT nodes_written int8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A hash pointer's own bytes, as a value to store where a collection looks
-- for pointers; and the value a pointer's bytes name, read whole.
CREATE FUNCTION test_toast_pointer_bytes(anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_toast_pointer_value(bytea)
RETURNS bytea
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

-- The names of the branches the transaction has written.
CREATE FUNCTION test_branches_written()
RETURNS text[]
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

-- The refs' commit of the session branch, as the engine calls it: the
-- commit's id, or NULL when it was refused as empty.
CREATE FUNCTION test_refs_commit(message text, allow_empty bool)
RETURNS text
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

-- The entries a map has past its relation's length.
CREATE FUNCTION test_bmap_past_end(relnumber oid,
	OUT blkno int8, OUT phys int8, OUT epoch int8, OUT refs int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_epoch(relnumber oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_free_blocks()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_sweep_due()
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_unlink(relnumber oid)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_truncate(relnumber oid, nblocks int,
	done bool DEFAULT false)
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

-- The server's chunk store, from SQL: chunks are named by the server's
-- content hash, in hex.
CREATE FUNCTION test_chunk_put(data bytea)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Put a chunk as the transaction commits, from a callback of its own.
CREATE FUNCTION test_chunk_put_at_commit(data bytea)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- data's content hash, as hex: by the implementation named (fed in pieces
-- of piece bytes, if given), or by the server's, which names chunks.
CREATE FUNCTION test_content_hash(data bytea, impl text DEFAULT NULL,
								  piece int DEFAULT NULL)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Whether every BLAKE3 SIMD variant built and runnable here agrees with
-- the portable code.
CREATE FUNCTION test_blake3_variants()
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C;

-- len bytes no compressor makes smaller, the same for the same seed.
CREATE FUNCTION test_noise(seed text, len int)
RETURNS bytea
LANGUAGE sql IMMUTABLE STRICT
AS $$
	SELECT substr(decode(string_agg(md5(seed || ' ' || i), '' ORDER BY i), 'hex'), 1, len)
	FROM generate_series(0, len / 16) i
$$;

CREATE FUNCTION test_chunk_get(hash text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Get several chunks at once (get_many()): each request's bytes (NULL:
-- none), and how many times it was answered.
CREATE FUNCTION test_chunk_get_many(hashes text[], OUT i int, OUT data bytea,
									OUT visits int)
RETURNS SETOF record
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

-- Conjoin the table files: all of them, or (max_files) the smallest of
-- those past max_files.
CREATE FUNCTION test_chunk_conjoin(max_files int DEFAULT 0)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_chunk_table_files(OUT name text, OUT chunks int,
	OUT bytes bigint, OUT settled boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C;

-- The journal's segments, oldest first (settled is always false).
CREATE FUNCTION test_chunk_segments(OUT name text, OUT chunks int,
	OUT bytes bigint, OUT settled boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Seal the journal's segment: appends go on to a new one.
CREATE FUNCTION test_chunk_seal()
RETURNS void
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
-- Lose a table's change-log entries the snapshot sees, its baseline kept:
-- a fold from it then misses their rows' changes.
CREATE FUNCTION test_lose_change_log(tbl regclass) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The session's last fold of a table in full: rows read, whether its sort
-- spilled to disk, and its sort's memory (or disk) in kB.
CREATE FUNCTION test_last_full_fold(OUT rows_read int8, OUT spilled bool,
	OUT sort_kb int8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Drop a prolly table's change log, as a table from before logs has none.
CREATE FUNCTION test_drop_change_log(tbl regclass) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_fold_counters(reset bool DEFAULT false, OUT pages int8,
	OUT candidates int8, OUT full_tables int8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The refs, below the version-control functions: a commit of the working
-- set with a parent given (as a merge's commit is made), and a head moved
-- with nothing else (as a fast-forward's).
CREATE FUNCTION test_commit_with(parent text)
RETURNS text
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

-- The row applier given rows to insert together (nested: in the first
-- subtransaction), then the first again twice, each in a subtransaction:
-- the errors caught, or "written".
CREATE FUNCTION test_apply_failed_load(rows anyarray, nested bool)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Add a chunk to a remote under the hash given, whatever its bytes hash to.
CREATE FUNCTION test_remote_put(url text, hash text, data bytea)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Make a new branch of a remote's at commit, recording format if it has none.
-- The same, the chunk given as stored (framed), as it is.
CREATE FUNCTION test_remote_put_stored(url text, hash text, frame bytea)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A remote's chunk as it gives it: as stored, framed (NULL: none).
CREATE FUNCTION test_remote_get_stored(url text, hash text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A chunk of the store as stored, framed (NULL: none); data framed so.
CREATE FUNCTION test_chunk_stored(hash text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_frame(data bytea, compress boolean)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A walk's map checked: n hashes in budget bytes; the runs it wrote.
CREATE FUNCTION test_seen_check(n int, budget int)
RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A walk's map checked against one in memory, n hashes in budget bytes:
-- what it cost.
CREATE FUNCTION test_seen_measure(n int, budget int,
	OUT memory_peak bigint, OUT filter_bytes_peak bigint,
	OUT filter_budget bigint, OUT runs_peak int, OUT runs_written bigint,
	OUT max_filter_checks int, OUT lookups bigint, OUT filter_checks bigint,
	OUT run_searches bigint, OUT false_positives bigint,
	OUT record_reads bigint, OUT merges bigint, OUT records_merged bigint,
	OUT temp_bytes_peak bigint, OUT block_reads bigint)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Every chunk a commit's walk reaches, once.
CREATE FUNCTION test_commit_chunks(commit_id text)
RETURNS SETOF text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_remote_set_head(url text, branch text, commit_id text,
	format text)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A folded root without its "!:format" item, as an older build made it.
CREATE FUNCTION test_root_without_format(root text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A folded root with its "!:format" item set, as a build of that format
-- made it.
CREATE FUNCTION test_root_with_format(root text, format text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A folded root whose sequences have no values recorded.
CREATE FUNCTION test_root_without_sequence_values(root text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A commit with no parents naming root and recording the dependencies
-- given (names and values, as the canonicalizer visits them).
CREATE FUNCTION test_commit_needing(root text, names text[], vals text[])
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Check what the commits record they need, as a clone (install) or a move.
CREATE FUNCTION test_check_dependencies(commits text[], install boolean)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- What a commit records its versioned objects needed from outside, each
-- "name => value"; NULL if it doesn't record them.
CREATE FUNCTION test_commit_dependencies(commit_id text)
RETURNS text[]
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A commit with no parents naming root.
CREATE FUNCTION test_commit_of(root text, message text)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- n commits in a line after parent, each naming its root; the last one.
CREATE FUNCTION test_commit_chain(parent text, n int)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The height a commit records; NULL if it records none.
CREATE FUNCTION test_commit_height(commit_id text)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A commit's height as the history walk finds it, and the commits it read.
CREATE FUNCTION test_history_height(commit_id text,
	OUT height int8, OUT reads int)
RETURNS record
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

-- Put a tree node naming chunks (level 0: values stored out of line in
-- them; above: children); its hash.
CREATE FUNCTION test_chunk_put_node(level int, refs text[])
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Put a leaf whose values are these bytes, inline, declaring no
-- references; its hash.
CREATE FUNCTION test_chunk_put_leaf(vals bytea[])
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Collect the chunk store's garbage (full: all of it): the chunks kept and
-- freed of those collected, the bytes freed, and how long (microseconds)
-- appends were held off.
-- A tree of one item ('k' -> 'v') whose item declares the chunks named
-- by hashes; its root's hash.
CREATE FUNCTION test_chunk_declaring_tree(hashes text[])
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_chunk_gc(full boolean, OUT kept int8, OUT freed int8,
	OUT bytes_freed int8, OUT window_usec int8)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The oldest position a logical slot of the current database, or one of a
-- standby's as its feedback said, may decode from (NULL: none): what a
-- collection keeps the values the WAL names from.
CREATE FUNCTION test_logical_horizon()
RETURNS pg_lsn
AS 'MODULE_PATHNAME' LANGUAGE C;

-- How many nodes the refs' walk from commit "to" visits that the walk
-- from commit "from" doesn't: what "to" added to the chunk store.
CREATE FUNCTION test_walk_new_count(from_commit text, to_commit text)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- What the value memo's lookups found since the last reset.
CREATE FUNCTION test_memo_counters(reset bool DEFAULT false, OUT hits int8,
	OUT misses int8)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C;

-- Run a statement with the rows it writes taken as written from a commit:
-- not refused for their keys' length, as the row applier's aren't.
CREATE FUNCTION test_write_held_rows(sql text) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The "basic" table file format's checks of an index, each damage with its
-- CRC made again; and a file of another version told from a damaged one.
CREATE FUNCTION test_tablefile_basic_index()
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C;

-- The bytes the "basic" table file format keeps in memory for each chunk
-- of an open file of n chunks named by 20-byte hashes: its index read, or
-- (mapped) used where the file has it.
CREATE FUNCTION test_tablefile_bytes_per_chunk(n int, mapped bool DEFAULT false)
RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The entries of a table's change log the transaction sees (its own
-- unwritten ones written first): how many were read, their deltas, and
-- whether one says to read the table in full (the read stops there).
CREATE FUNCTION test_log_entries(tbl regclass, OUT n int8, OUT deltas int8[],
	OUT full_read bool)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Give a table, its change log dropped, a log of the first format (a row an
-- entry, no index), as a table logged before the format changed has.
CREATE FUNCTION test_make_format1_change_log(tbl regclass) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
