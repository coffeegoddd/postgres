-- Moving a table without a primary key (a reset, a branch made from one
-- with changes) finds the rows it removes or changes by their values: the
-- table is scanned for them all at once, not once for each.  Rows alike
-- (duplicates) are told apart, and a row the index misses is still found.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA km VERSIONED;
CREATE TABLE km.t (a int, b text, c numeric);
INSERT INTO km.t SELECT g % 50, 'row ' || (g % 50), g % 7 FROM generate_series(1, 500) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- 300 rows added, 200 removed (duplicates among them), 100 changed
INSERT INTO km.t SELECT g, 'new ' || g, 1.5 FROM generate_series(1, 300) g;
DELETE FROM km.t WHERE ctid IN (SELECT ctid FROM km.t WHERE a < 20 LIMIT 200);
UPDATE km.t SET b = b || '!' WHERE a BETWEEN 30 AND 39;

BEGIN;
SELECT dolt_reset('--hard');
SELECT seq_scan < 20 AS scanned_once FROM pg_stat_xact_user_tables
  WHERE relid = 'km.t'::regclass;
COMMIT;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT count(*), count(DISTINCT (a, b, c)) FROM km.t;

-- A branch made while main has such changes starts at the head too.
INSERT INTO km.t SELECT g, 'more', 2 FROM generate_series(1, 400) g;
SELECT dolt_branch('kb');
SELECT dolt_checkout('kb') IS NOT NULL AS on_kb;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS at_head;
SELECT count(*) FROM km.t;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT count(*) FROM km.t;

SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'kb');
DROP SCHEMA km CASCADE;
