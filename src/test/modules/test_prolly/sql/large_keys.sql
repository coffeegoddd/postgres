-- A row's key is bounded as the row is written.  A commit keys each row of
-- a table with a primary key by the key columns, encoded and uncompressed,
-- and a tree holds keys of at most 32680 bytes (a text key: 32678 bytes of
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
INSERT INTO lk.t VALUES (repeat('z', 32679), 0);
INSERT INTO lk.t SELECT chr(96 + g) || repeat('a', 32677), g
  FROM generate_series(1, 6) g;
SELECT dolt_commit('-Am', 'longest keys') IS NOT NULL AS committed;
SELECT count(*), min(length(k)), max(length(k)) FROM lk.t;

-- An update to a key too long; others go on, the longest keys' rows too.
UPDATE lk.t SET k = k || 'x' WHERE n = 1;
UPDATE lk.t SET n = n + 10;
INSERT INTO lk.t VALUES ('a' || repeat('a', 32677), 0)
  ON CONFLICT (k) DO UPDATE SET n = EXCLUDED.n;
INSERT INTO lk.t VALUES ('b' || repeat('a', 32678), 0)
  ON CONFLICT (k) DO UPDATE SET n = EXCLUDED.n;
SELECT dolt_commit('-am', 'updated') IS NOT NULL AS committed;

-- COPY (heap_multi_insert).
\getenv abs_builddir PG_ABS_BUILDDIR
\set long_file :abs_builddir '/results/large_keys_long.data'
\set ok_file :abs_builddir '/results/large_keys_ok.data'
COPY (SELECT 'c' || repeat('q', 40000), 1) TO :'long_file';
COPY (SELECT 'c' || repeat('q', 32677), 1) TO :'ok_file';
COPY lk.t FROM :'long_file';
COPY lk.t FROM :'ok_file';

-- A key of several columns, and one of a type encoded as its bytes.
CREATE TABLE lk.multi (a int, b text, c text, PRIMARY KEY (a, b, c));
INSERT INTO lk.multi VALUES (1, repeat('m', 16000), repeat('m', 16000));
INSERT INTO lk.multi VALUES (2, repeat('m', 20000), repeat('m', 20000));
CREATE TABLE lk.bytes (b bytea PRIMARY KEY);
INSERT INTO lk.bytes VALUES (decode(repeat('ab', 32675), 'hex'));
INSERT INTO lk.bytes VALUES (decode(repeat('ab', 32676), 'hex'));

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
INSERT INTO lk.deferred VALUES (repeat('d', 32678));
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

SET client_min_messages = warning;
DROP SCHEMA lk CASCADE;
