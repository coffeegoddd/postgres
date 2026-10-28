-- Restoring only the pages the WAL lists is exact only if every page change
-- after the base point is in the WAL and the WAL is read far enough.
-- btree marks dead entries (LP_DEAD) as a hint: logged only with checksums
-- or wal_log_hints, and only as a full page image for the first change
-- after a checkpoint, and a read-only query never flushes that record.
-- So: hints logged, a checkpoint at the base point, and a forced flush
-- before reading the changed pages.
-- Run against a server with pageinspect, pg_walinspect, amcheck, prolly_pages.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
DROP TABLE IF EXISTS r;
CREATE TABLE r (id bigint PRIMARY KEY, pad text) WITH (autovacuum_enabled = false);
INSERT INTO r SELECT g, lpad(g::text, 200, 'x') FROM generate_series(1, 20000) g;
VACUUM (FREEZE) r;
-- base point: capture every page, then checkpoint
DROP TABLE IF EXISTS img;
CREATE TABLE img AS
  SELECT 'r'::regclass AS rel, b, get_raw_page('r', 'main', b) AS p
    FROM generate_series(0, pg_relation_size('r') / 8192 - 1) b
  UNION ALL
  SELECT 'r_pkey'::regclass, b, get_raw_page('r_pkey', 'main', b)
    FROM generate_series(0, pg_relation_size('r_pkey') / 8192 - 1) b;
SELECT pg_current_wal_flush_lsn() AS base \gset
CHECKPOINT;
-- change: delete rows, then let an index scan mark their entries dead
DELETE FROM r WHERE id % 50 = 0;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS after_delete FROM r WHERE id > 0;
SELECT count(*) AS scan_marks_dead FROM r WHERE id % 50 = 0 AND id > 0;
SELECT count(*) FILTER (WHERE dead) AS dead_before_restore
  FROM generate_series(1, pg_relation_size('r_pkey')/8192 - 1) b, bt_page_items('r_pkey', b::int);
SELECT record_type AS pkey_wal, count(*) FROM pg_get_wal_block_info(:'base', pg_current_wal_flush_lsn(), false)
 WHERE relfilenode = pg_relation_filenode('r_pkey') GROUP BY 1;
-- restore just the pages the WAL says changed since the base; reading
-- the WAL up to the flushed position needs those hint records flushed
SELECT pg_logical_emit_message(false, 'prolly', '', true) IS NOT NULL AS flushed;
BEGIN;
SELECT rel, prolly_put_pages(rel, 'main', array_agg(i.b::int4 ORDER BY i.b),
                             string_agg(i.p, ''::bytea ORDER BY i.b)) AS restored
  FROM img i
  JOIN (SELECT DISTINCT relfilenode, relblocknumber
          FROM pg_get_wal_block_info(:'base', pg_current_wal_flush_lsn(), false)
         WHERE relforknumber = 0) w
    ON w.relfilenode = pg_relation_filenode(i.rel) AND w.relblocknumber = i.b
 GROUP BY rel ORDER BY rel;
COMMIT;
SHOW wal_log_hints;
SELECT count(*) AS via_pkey_should_be_20000 FROM r WHERE id > 0;
RESET enable_seqscan; RESET enable_bitmapscan;
SELECT count(*) AS via_heap FROM r;
