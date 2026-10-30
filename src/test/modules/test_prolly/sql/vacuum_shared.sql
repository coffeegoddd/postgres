-- VACUUM on block-mapped pages another relation shares.  Preparing a
-- shared page for a change copies it, so VACUUM leaves a shared page with
-- nothing on it to reclaim as it is, and copies only the pages it must
-- change (dead tuples).  Block-mapped tables freeze eagerly, while their
-- pages are still their own, so a share made later rarely needs copying.
CREATE EXTENSION IF NOT EXISTS test_prolly;
CREATE EXTENSION IF NOT EXISTS pg_visibility;
CREATE EXTENSION IF NOT EXISTS pg_walinspect;
SET debug_block_mapped_heap = on;
SET vacuum_freeze_min_age TO DEFAULT;

-- the logical blocks whose physical block two relations' maps both name
CREATE FUNCTION shared_blocks(a oid, b oid) RETURNS bigint
LANGUAGE sql AS $$
	SELECT count(*) FROM test_bmap_map(a) x JOIN test_bmap_map(b) y
	  ON x.blkno = y.blkno AND x.phys = y.phys
$$;

-- An unvacuumed table shared with another relation: VACUUM finds nothing
-- to reclaim on the shared pages and leaves every one of them shared,
-- though it would otherwise freeze them and set them all-visible.
CREATE TABLE shared_t (id int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO shared_t SELECT g, repeat('s', 200) FROM generate_series(1, 3000) g;
SELECT relfilenode AS t FROM pg_class WHERE relname = 'shared_t' \gset
SELECT pg_relation_size('shared_t') / 8192 AS nblocks \gset
SELECT test_bmap_share(:t) AS copy \gset
SELECT shared_blocks(:t, :copy) = :nblocks AS all_shared;
-- reading the table, which sets hint bits and may prune pages on access,
-- copies none of them
SELECT count(*) FROM shared_t;
SELECT shared_blocks(:t, :copy) = :nblocks AS read_copies_nothing;
VACUUM shared_t;
SELECT shared_blocks(:t, :copy) = :nblocks AS all_still_shared;
SELECT count(*) FROM shared_t;

-- Dead tuples on a shared page are reclaimed: that page is copied, and
-- only that one.
DELETE FROM shared_t WHERE id <= 10;
VACUUM shared_t;
SELECT :nblocks - shared_blocks(:t, :copy) AS pages_copied;
SELECT count(*) FROM shared_t;
SELECT test_bmap_unlink(:copy);

-- A table vacuumed before it is shared is frozen then, in place, so a
-- VACUUM after the share has nothing to freeze and copies nothing.
CREATE TABLE frozen_t (id int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO frozen_t SELECT g, repeat('f', 200) FROM generate_series(1, 3000) g;
VACUUM frozen_t;
SELECT relfilenode AS f FROM pg_class WHERE relname = 'frozen_t' \gset
SELECT pg_relation_size('frozen_t') / 8192 AS fblocks \gset
SELECT count(*) = :fblocks AS all_frozen
FROM generate_series(0, :fblocks - 1) b
WHERE (SELECT all_frozen FROM pg_visibility_map('frozen_t', b));
SELECT test_bmap_share(:f) AS fcopy \gset
VACUUM (FREEZE) frozen_t;
SELECT shared_blocks(:f, :fcopy) = :fblocks AS frozen_still_shared;
SELECT test_bmap_unlink(:fcopy);

-- A shared page that says it is all-visible and is all-frozen, but whose
-- map bits were cleared (the map emptied here), gets them back from VACUUM
-- without being copied: a record of the map page alone, VM_SET, for each.
CREATE TABLE vmset_t (id int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO vmset_t SELECT g, repeat('v', 200) FROM generate_series(1, 3000) g;
VACUUM (FREEZE) vmset_t;
SELECT relfilenode AS v FROM pg_class WHERE relname = 'vmset_t' \gset
SELECT pg_relation_size('vmset_t') / 8192 AS vblocks \gset
SELECT pg_truncate_visibility_map('vmset_t');
SELECT count(*) AS bits_left FROM pg_visibility_map('vmset_t') WHERE all_visible;
SELECT count(*) = :vblocks AS pages_say_all_visible
FROM pg_visibility('vmset_t') WHERE pd_all_visible;
SELECT test_bmap_share(:v) AS vcopy \gset
SELECT pg_current_wal_insert_lsn() AS vm_mark \gset
VACUUM vmset_t;
SELECT count(*) = :vblocks AS bits_back FROM pg_visibility_map('vmset_t')
WHERE all_visible AND all_frozen;
SELECT shared_blocks(:v, :vcopy) = :vblocks AS vm_set_copies_nothing;
-- (WAL flushed by a transaction that logs a change: the VACUUM's commits none)
CREATE TABLE vm_set_flushed ();
DROP TABLE vm_set_flushed;
SELECT count(*) = :vblocks AS vm_set_records
FROM pg_get_wal_block_info(:'vm_mark', pg_current_wal_flush_lsn())
WHERE resource_manager = 'Heap2' AND record_type = 'VM_SET'
  AND relfilenode = :v AND relforknumber = 2;
SELECT test_bmap_unlink(:vcopy);

-- A page shared only with a root held of the relation itself, not with
-- another relation, is vacuumed as the relation's own: frozen and set
-- all-visible, copied, while the root keeps the version it had.
CREATE TABLE held_t (id int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO held_t SELECT g, repeat('h', 200) FROM generate_series(1, 3000) g;
SELECT relfilenode AS h FROM pg_class WHERE relname = 'held_t' \gset
SELECT pg_relation_size('held_t') / 8192 AS hblocks \gset
BEGIN;
SELECT test_bmap_root_capture(:h) AS root \gset
COMMIT;
VACUUM held_t;
SELECT count(*) = :hblocks AS held_pages_frozen
FROM generate_series(0, :hblocks - 1) b
WHERE (SELECT all_frozen FROM pg_visibility_map('held_t', b));
BEGIN;
SELECT test_bmap_root_release(:root);
COMMIT;

DROP TABLE shared_t, frozen_t, vmset_t, held_t;
DROP FUNCTION shared_blocks(oid, oid);
SELECT entries >= 0 AS drained FROM test_bmap_release_drain();
RESET debug_block_mapped_heap;
