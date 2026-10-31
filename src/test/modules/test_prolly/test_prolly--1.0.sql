/* src/test/modules/test_prolly/test_prolly--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_prolly" to load this file. \quit

-- Run the unit tests of the engine's code, or those of one suite.
CREATE FUNCTION test_prolly_unit_tests(suite_name text DEFAULT NULL,
	OUT suite text, OUT test text, OUT result text,
	OUT detail text, OUT location text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Run every interface's conformance suite, or one interface's, against each
-- registered implementation of it.
CREATE FUNCTION test_prolly_conformance(iface_name text DEFAULT NULL,
	OUT iface text, OUT implementation text, OUT test text,
	OUT result text, OUT detail text, OUT location text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Whether the module was loaded through shared_preload_libraries, rather
-- than by the first call of one of its functions.
CREATE FUNCTION test_prolly_loaded_at_preload()
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

-- Block-mapped storage, below the catalogs: storage is named by
-- relation file number in the database's default tablespace.  A copy
-- made by test_bmap_share() lasts until test_bmap_unlink() drops it, or,
-- if transactional, until the transaction aborts.
CREATE FUNCTION test_bmap_share(relnumber oid, transactional bool DEFAULT false)
RETURNS oid
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_digest(relnumber oid)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- (fork 0, the main fork, or 2, its visibility map)
CREATE FUNCTION test_bmap_map(relnumber oid, fork int4 DEFAULT 0,
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

CREATE FUNCTION test_bmap_exists(relnumber oid)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_free_blocks()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The slot following the page store's flushes for shares and captures.
CREATE FUNCTION test_bmap_store_sync(OUT written int8, OUT flushing int4)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_sweep_due()
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_drop_orphans()
RETURNS int4
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- drain: the release queue drained after, as a drop's blocks were freed
-- before the queue (false leaves them queued)
CREATE FUNCTION test_bmap_unlink(relnumber oid, drain bool DEFAULT true)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- the release queue drained, of at most max_entries entries (0: all)
CREATE FUNCTION test_bmap_release_drain(max_entries int8 DEFAULT 0,
	OUT entries int8, OUT freed int8, OUT unrefs int8)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The page store swept: blocks nothing holds freed, their counts zeroed,
-- the store shortened, and the blocks moved down for that.
CREATE FUNCTION test_bmap_sweep(OUT freed int8, OUT recounted int8,
	OUT truncated int8, OUT moved int8)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_release_pending()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_truncate(relnumber oid, nblocks int,
	done bool DEFAULT false)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_extend(relnumber oid, n int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Change a byte of a page's hole, as an access method changes a page.
CREATE FUNCTION test_bmap_write(relnumber oid, blkno int8, value int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Write an empty page past the end, as a hash index's bucket allocation
-- writes the last of its blocks, leaving those between unwritten.
CREATE FUNCTION test_bmap_extend_at(relnumber oid, blkno int8)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Initialize a block below the end, as an access method makes a new page.
CREATE FUNCTION test_bmap_init_block(relnumber oid, blkno int8)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Read a page, through shared buffers or not: 'zeroes', or the byte
-- test_bmap_write() changes.
CREATE FUNCTION test_bmap_read(relnumber oid, blkno int8, buffered bool)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The logical blocks that have a physical block, found from the map's nodes.
CREATE FUNCTION test_bmap_mapped(relnumber oid,
	OUT blkno int8, OUT phys int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A digest of heap pages' rows, leaving out hint bits.
CREATE FUNCTION test_bmap_rows_digest(relnumber oid)
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The bytes of WAL this backend has written, leaving out full-page images.
CREATE FUNCTION test_bmap_own_wal()
RETURNS int8
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

-- Drop a block's page from the buffer pool without writing it.
CREATE FUNCTION test_bmap_drop_buffer(relnumber oid, blkno int)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Each of a relation's cached pages checked again before it next changes,
-- as when its pages begin to be shared.
CREATE FUNCTION test_bmap_mark_shared(relnumber oid)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A map's nodes, each time one is reached from the root page.
CREATE FUNCTION test_bmap_tree(relnumber oid,
	OUT level int4, OUT first int8, OUT block int8, OUT refs int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Roots captured into the database's page store: captured (an id), let go
-- of as the transaction commits, asked of (the length it holds, NULL if not
-- held), copied into a new relation file number (its main fork alone, which
-- test_bmap_unlink() drops), counted (those held, and the slots), and the
-- slots of captures that never committed reclaimed; and those held captured
-- of a relation file number.
-- Say in pg_control that pages were synced instead of logged, relying on
-- no recovery beginning at or before 'lsn'.
CREATE FUNCTION test_synced_pages_note(lsn pg_lsn)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_root_capture(relnumber oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A frozen root captured of storage 'copy', recorded as of 'of' at its
-- epoch now.
CREATE FUNCTION test_bmap_root_capture_of(copy oid, "of" oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_root_release(id int8)
RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_root_held(id int8)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_share_root(id int8)
RETURNS oid
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_roots(OUT held int8, OUT slots int8)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_roots_of(relnumber oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The captured roots' slots in use that name a transaction, as capturer
-- or releaser (VACUUM settles them, so that none needs the commit log).
CREATE FUNCTION test_bmap_roots_naming()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- How many times this backend asked the file system whether a reference
-- counts' file exists (md.c closing and opening it again) or its length
-- (a seek).
CREATE FUNCTION test_bmap_refs_file_asks()
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The standby hold published (NULL: none): the earliest position a
-- recovery of a standby known may begin at, and whether one runs a backup.
CREATE FUNCTION test_bmap_standby_hold(OUT hold pg_lsn, OUT backups bool)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_root_counts(OUT whole bool, OUT counted int8,
	OUT differ int8)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_roots_reclaim(settle bool DEFAULT false)
RETURNS int4
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Look up every logical block of the storage 'loops' times, as reading its
-- pages does (from cached map pages): the microseconds it took.
CREATE FUNCTION test_bmap_lookups(relnumber oid, loops int4)
RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The block map's primitives: storage made from a held root (with copies
-- of another relation's free space and visibility maps, unless 0), an
-- epoch raised and the pages changed since one, and whether a page is
-- shared, would be copied, and may be held by another relation.
CREATE FUNCTION test_bmap_storage_from_root(root int8, maps_from oid DEFAULT 0)
RETURNS oid
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_raise_epoch(relnumber oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_changed_pages(relnumber oid, since int8)
RETURNS SETOF int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_page_shared(relnumber oid, blkno int8,
	OUT shared bool, OUT copies bool, OUT elsewhere bool)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_diff(a oid, b oid)
RETURNS SETOF int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_diff_root(relnumber oid, root int8)
RETURNS SETOF int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_diff_roots(a int8, b int8)
RETURNS SETOF int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_bmap_diff_reads(a oid, b oid)
RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
