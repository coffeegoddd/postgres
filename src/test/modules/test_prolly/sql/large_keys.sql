-- A row's key is bounded as the row is written.  A commit keys each row of
-- a table with a primary key by the key columns, encoded and uncompressed,
-- and a tree holds keys of at most 32712 bytes (a text key: 32710 bytes of
-- text, a marker and a terminator).  btree takes longer keys, compressed,
-- so a row with one used to be written, and every commit of its branch
-- failed after.  Now the write is refused: INSERT, COPY, UPDATE, and a
-- primary key made over rows written before, by its index's build or of
-- an index built before.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA lk VERSIONED;
CREATE TABLE lk.t (k text PRIMARY KEY, n int);

-- The issue's row, and the bound's edges.
INSERT INTO lk.t VALUES (repeat('z', 100000), 0);
INSERT INTO lk.t VALUES (repeat('z', 32711), 0);
INSERT INTO lk.t SELECT chr(96 + g) || repeat('a', 32709), g
  FROM generate_series(1, 6) g;
SELECT dolt_commit('-Am', 'longest keys') IS NOT NULL AS committed;
SELECT count(*), min(length(k)), max(length(k)) FROM lk.t;

-- An update to a key too long; others go on, the longest keys' rows too.
UPDATE lk.t SET k = k || 'x' WHERE n = 1;
UPDATE lk.t SET n = n + 10;
INSERT INTO lk.t VALUES ('a' || repeat('a', 32709), 0)
  ON CONFLICT (k) DO UPDATE SET n = EXCLUDED.n;
INSERT INTO lk.t VALUES ('b' || repeat('a', 32710), 0)
  ON CONFLICT (k) DO UPDATE SET n = EXCLUDED.n;
SELECT dolt_commit('-am', 'updated') IS NOT NULL AS committed;

-- COPY (heap_multi_insert).
\getenv abs_builddir PG_ABS_BUILDDIR
\set long_file :abs_builddir '/results/large_keys_long.data'
\set ok_file :abs_builddir '/results/large_keys_ok.data'
COPY (SELECT 'c' || repeat('q', 40000), 1) TO :'long_file';
COPY (SELECT 'c' || repeat('q', 32709), 1) TO :'ok_file';
COPY lk.t FROM :'long_file';
COPY lk.t FROM :'ok_file';

-- A key of several columns, and one of a type encoded as its bytes.
CREATE TABLE lk.multi (a int, b text, c text, PRIMARY KEY (a, b, c));
INSERT INTO lk.multi VALUES (1, repeat('m', 16000), repeat('m', 16000));
INSERT INTO lk.multi VALUES (2, repeat('m', 20000), repeat('m', 20000));
CREATE TABLE lk.bytes (b bytea PRIMARY KEY);
INSERT INTO lk.bytes VALUES (decode(repeat('ab', 32707), 'hex'));
INSERT INTO lk.bytes VALUES (decode(repeat('ab', 32708), 'hex'));

-- Rows keyed by content take any size: a table without a primary key, and
-- one whose key is deferrable (it may hold duplicates until commit).
CREATE TABLE lk.none (k text);
INSERT INTO lk.none VALUES (repeat('n', 100000));
CREATE TABLE lk.deferred (k text PRIMARY KEY DEFERRABLE);
INSERT INTO lk.deferred VALUES (repeat('d', 100000));

-- A primary key added over rows written without one: its index's build
-- checks them (a column's missing value too), and the rewrite of a column
-- type rebuilds it.
ALTER TABLE lk.none ADD PRIMARY KEY (k);
CREATE TABLE lk.added (a int);
INSERT INTO lk.added VALUES (1);
ALTER TABLE lk.added ADD COLUMN k text NOT NULL DEFAULT repeat('a', 40000);
SELECT atthasmissing FROM pg_attribute  -- (the row holds no k: its default)
  WHERE attrelid = 'lk.added'::regclass AND attname = 'k';
ALTER TABLE lk.added ADD PRIMARY KEY (k);
CREATE TABLE lk.retyped (k varchar(40000) PRIMARY KEY);
INSERT INTO lk.retyped VALUES (repeat('r', 30000));
ALTER TABLE lk.retyped ALTER COLUMN k TYPE text USING k || k;

-- ... or of a unique index built before.
CREATE UNIQUE INDEX none_k ON lk.none (k);
ALTER TABLE lk.none ADD CONSTRAINT none_pk PRIMARY KEY USING INDEX none_k;
ALTER TABLE lk.deferred DROP CONSTRAINT deferred_pkey;
CREATE UNIQUE INDEX deferred_k ON lk.deferred (k);
DELETE FROM lk.deferred;
INSERT INTO lk.deferred VALUES (repeat('d', 32710));
ALTER TABLE lk.deferred ADD CONSTRAINT deferred_pk PRIMARY KEY USING INDEX deferred_k;

-- None of it stops a commit, and every row comes back from it.
SELECT dolt_commit('-Am', 'the rest') IS NOT NULL AS committed;
SELECT 't' AS tbl, count(*), max(length(k)) FROM lk.t
UNION ALL SELECT 'multi', count(*), max(length(b) + length(c)) FROM lk.multi
UNION ALL SELECT 'bytes', count(*), max(length(b)) FROM lk.bytes
UNION ALL SELECT 'none', count(*), max(length(k)) FROM lk.none
UNION ALL SELECT 'deferred', count(*), max(length(k)) FROM lk.deferred
UNION ALL SELECT 'retyped', count(*), max(length(k)) FROM lk.retyped
ORDER BY 1;
SELECT dolt_hashof('main') AS head \gset
SELECT dolt_reset('--hard', 'HEAD~2') IS NOT NULL AS reset;
SELECT count(*), max(length(k)) FROM lk.t;
SELECT dolt_reset('--hard', :'head') IS NOT NULL AS reset;
SELECT count(*), sum(length(k)) FROM lk.t;
SELECT count(*), max(length(k)) FROM lk.deferred;

-- A commit made before the bound was may hold keys a little longer than
-- it (the bound takes a child's count to be as long as any, and a tree's
-- were shorter).  The rows the row applier writes from a tree a commit
-- held aren't refused, so the branch is moved to that commit, and it is
-- reverted and cherry-picked, as any other.  (Its rows written here as the
-- applier writes them.)  Written anew, they're refused as any row; updated
-- keeping their keys, they aren't.
CREATE TABLE lk.held (k text PRIMARY KEY, n int);
INSERT INTO lk.held VALUES (repeat('h', 32715), 0);
SELECT test_write_held_rows($$INSERT INTO lk.held
  SELECT chr(96 + g) || repeat('h', 32714), g FROM generate_series(1, 7) g$$);
SELECT dolt_commit('-Am', 'held keys') IS NOT NULL AS committed;
SELECT dolt_branch('held_old');
DELETE FROM lk.held;
SELECT dolt_commit('-am', 'held keys deleted') IS NOT NULL AS committed;
SELECT dolt_hashof('main') AS deleted \gset
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT count(*), max(octet_length(k)) FROM lk.held;
SELECT dolt_reset('--hard', :'deleted') IS NOT NULL AS reset;
SELECT count(*) FROM lk.held;
SELECT dolt_checkout('held_old') IS NOT NULL AS checked_out;
SELECT count(*), max(octet_length(k)) FROM lk.held;
SELECT dolt_checkout('main') IS NOT NULL AS checked_out;
SELECT dolt_revert('HEAD') IS NOT NULL AS reverted;
SELECT count(*), max(octet_length(k)) FROM lk.held;
SELECT dolt_hashof('main') AS reverted \gset
SELECT dolt_reset('--hard', 'HEAD~1') IS NOT NULL AS reset;
SELECT dolt_cherry_pick(:'reverted') IS NOT NULL AS picked;
SELECT count(*), max(octet_length(k)) FROM lk.held;
UPDATE lk.held SET n = n + 10 WHERE n <= 2;
INSERT INTO lk.held VALUES ('c' || repeat('h', 32714), 3)
  ON CONFLICT (k) DO UPDATE SET n = EXCLUDED.n + 20;
UPDATE lk.held SET k = k || 'h' WHERE n = 4;
UPDATE lk.held SET k = left(k, 32000) WHERE n = 5;
DELETE FROM lk.held WHERE n = 11;
SELECT n, octet_length(k) FROM lk.held ORDER BY n;
SELECT dolt_commit('-am', 'one held key deleted') IS NOT NULL AS committed;
SELECT dolt_branch('-D', 'held_old') IS NOT NULL AS deleted;

-- A dropped table's keying isn't kept (only one invalidated since the
-- last row checked may be).
SELECT count(*) AS bounds FROM pg_backend_memory_contexts
  WHERE name = 'prolly key bound' \gset
DO $$
BEGIN
  FOR i IN 1..20 LOOP
    CREATE TABLE lk.dropped (k int PRIMARY KEY, v text);
    INSERT INTO lk.dropped VALUES (i, 'x');
    DROP TABLE lk.dropped;
  END LOOP;
END $$;
SELECT count(*) - :bounds <= 1 AS freed FROM pg_backend_memory_contexts
  WHERE name = 'prolly key bound';

SET client_min_messages = warning;
DROP SCHEMA lk CASCADE;
