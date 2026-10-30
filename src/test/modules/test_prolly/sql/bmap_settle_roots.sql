-- A block-mapped page store's captured roots name the transactions that
-- captured them.  Before the database's oldest transaction id advances,
-- and the commit log may forget those transactions, VACUUM settles every
-- root: one held names no transaction any more, and one whose capturer
-- aborted is reclaimed.  Otherwise, once the log can't say a capturer
-- aborted, its root would be held for good.  (Whether the roots name
-- their capturers before the VACUUM isn't checked: autovacuum may settle
-- them first, in vac_update_datfrozenxid().)
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET debug_block_mapped_heap = on;
CREATE TABLE settle_t (id int) WITH (autovacuum_enabled = off);
INSERT INTO settle_t SELECT generate_series(1, 1000);
SELECT relfilenode AS t FROM pg_class WHERE relname = 'settle_t' \gset

BEGIN;
SELECT test_bmap_root_capture(:t) AS kept \gset
COMMIT;
BEGIN;
SELECT test_bmap_root_capture(:t) > 0 AS captured_then_aborted;
ROLLBACK;

VACUUM settle_t;
SELECT test_bmap_roots_naming() AS roots_naming_after_vacuum;
SELECT test_bmap_root_held(:kept) IS NOT NULL AS kept_root_still_held;

BEGIN;
SELECT test_bmap_root_release(:kept);
COMMIT;
DROP TABLE settle_t;
SELECT entries >= 0 AS drained FROM test_bmap_release_drain();
RESET debug_block_mapped_heap;
