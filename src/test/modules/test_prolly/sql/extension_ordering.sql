-- Extension opclasses ordering prolly keys require their exact version.
SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/ordering-' || txid_current() AS url \gset
CREATE DATABASE regress_ordering_src;
\c regress_ordering_src
CREATE SCHEMA tools;
CREATE EXTENSION test_prolly SCHEMA tools;
CREATE EXTENSION test_prolly_ordering VERSION '1.0';
CREATE SCHEMA ord VERSIONED;
CREATE TABLE ord.nonkey (id int PRIMARY KEY, k test_ordering);
CREATE TABLE ord.keyless (k test_ordering);
CREATE TABLE ord.deferred (k test_ordering PRIMARY KEY DEFERRABLE);
CREATE INDEX nonkey_idx ON ord.nonkey (k);
INSERT INTO ord.nonkey VALUES (1, ROW(10));
INSERT INTO ord.keyless VALUES (ROW(10));
INSERT INTO ord.deferred VALUES (ROW(10));
SELECT (dolt_commit('-Am', 'no ordered extension keys'))[1] AS nonkey \gset
SELECT replace(d, E'\t', ' | ') AS dependency
 FROM unnest(tools.test_commit_dependencies(:'nonkey')) d ORDER BY 1;

-- Either component of a composite primary key can require the guard.
CREATE TABLE ord.composite (id int, k test_ordering, n int, PRIMARY KEY (id, k));
INSERT INTO ord.composite VALUES (1, ROW(1), 10), (1, ROW(2), 20), (2, ROW(1), 30);
SELECT (dolt_commit('-Am', 'ordered extension keys'))[1] AS ordered \gset
SELECT replace(d, E'\t', ' | ') AS dependency
 FROM unnest(tools.test_commit_dependencies(:'ordered')) d ORDER BY 1;
SELECT tools.test_check_dependencies(ARRAY[:'ordered'], false);
CREATE TABLE ord.single (k test_ordering PRIMARY KEY, n int);
INSERT INTO ord.single VALUES (ROW(3), 3), (ROW(1), 1), (ROW(2), 2);
SELECT length(dolt_commit('-Am', 'single column key')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main');

-- A fresh clone installs the exact version, even though the default is newer.
\c :db
CREATE DATABASE regress_ordering_dst;
\c regress_ordering_dst
SELECT dolt_clone(:'url');
SELECT extversion FROM pg_extension WHERE extname = 'test_prolly_ordering';
SELECT * FROM ord.composite ORDER BY id, k;

-- An installed newer version is NOT compatible for key ordering.
\c :db
CREATE DATABASE regress_ordering_wrong;
\c regress_ordering_wrong
CREATE EXTENSION test_prolly_ordering;
SELECT dolt_clone(:'url');
SELECT to_regclass('ord.composite') IS NULL AS untouched;

\c regress_ordering_src
SELECT dolt_checkout('-b', 'side');
UPDATE ord.composite SET n = 21 WHERE id = 1 AND (k).v = 2;
SELECT (dolt_commit('-Am', 'side'))[1] AS side \gset
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'side');
SELECT dolt_branch('old', :'side');
SELECT dolt_checkout('-b', 'dropped');
DROP TABLE ord.composite, ord.single;
SELECT (dolt_commit('-Am', 'drop ordered keys'))[1] AS dropped \gset
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'dropped');
SELECT tools.test_fold_root() AS before_upgrade \gset
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
ALTER EXTENSION test_prolly_ordering UPDATE TO '1.1';
-- Postgres's native indexes also need rebuilding after a comparator change.
REINDEX TABLE ord.nonkey;
REINDEX TABLE ord.deferred;
REINDEX TABLE ord.composite;
REINDEX TABLE ord.single;
-- With no row writes, the changed ordering dependency forces a fresh fold.
SELECT tools.test_fold_root() AS after_upgrade \gset
SELECT :'after_upgrade' = tools.test_fold_root(true) AS same_as_full,
 :'after_upgrade' <> :'before_upgrade' AS reordered;
SELECT pg_stat_force_next_flush();
SELECT value > 0 AS changed_shape FROM pg_stat_prolly WHERE name = 'fold_full_shape_changed';
-- A non-key use (including a secondary index and deferrable PK) permits updates.
SELECT tools.test_check_dependencies(ARRAY[:'nonkey'], false);
-- Direct history reads must not bypass the move guard.
SELECT * FROM dolt_at(:'ordered', NULL::ord.composite);
SELECT * FROM dolt_diff(:'ordered', :'side', NULL::ord.composite);
SELECT * FROM dolt_at(:'nonkey', NULL::ord.nonkey);
-- Mismatches fail before changing the head or the rows.
SELECT dolt_checkout('old');
SELECT dolt_reset('--hard', :'side');
SELECT dolt_reset('--hard');
SELECT dolt_cherry_pick(:'side');
-- The incoming and merge-base dependencies are identical: still check ordering.
-- Auto-commit must refold rather than apply row changes to the stale head.
SET dolt.transaction_commit = on;
UPDATE ord.composite SET n = 31 WHERE id = 2;
SET dolt.transaction_commit = off;
SELECT dolt_hashof('HEAD') AS newer \gset
SELECT count(*) AS verify_mismatches FROM dolt_verify_folds() WHERE NOT matches;
SELECT dolt_merge(:'side');
-- The base still needs its old ordering when the incoming side drops the keys.
SELECT dolt_merge(:'dropped');
SELECT dolt_hashof('HEAD') = :'newer' AS unchanged;
SELECT * FROM ord.composite ORDER BY id, k;
SELECT replace(d, E'\t', ' | ') AS dependency
 FROM unnest(tools.test_commit_dependencies(:'newer')) d ORDER BY 1;

-- A clone with incompatible head requirements cannot choose the newest one.
SELECT dolt_push('origin', 'main');
SELECT dolt_push('origin', 'old');
\c :db
CREATE DATABASE regress_ordering_conflict;
\c regress_ordering_conflict
SELECT dolt_clone(:'url');
SELECT count(*) AS not_installed FROM pg_extension WHERE extname = 'test_prolly_ordering';

-- A minimum requirement must not silently override an exact ordering version.
CREATE SCHEMA tools;
CREATE EXTENSION test_prolly SCHEMA tools;
SELECT tools.test_commit_needing(tools.test_fold_root(), ARRAY['extension test_prolly_ordering'],
 ARRAY['{1.0,public,ordering}']) AS exact_old \gset
SELECT tools.test_commit_needing(tools.test_fold_root(), ARRAY['extension test_prolly_ordering'],
 ARRAY['{1.1,public}']) AS minimum_new \gset
SELECT tools.test_check_dependencies(ARRAY[:'exact_old', :'minimum_new'], true);
SELECT count(*) AS not_installed FROM pg_extension WHERE extname = 'test_prolly_ordering';
-- An older minimum plus a newer exact version is compatible.
SELECT tools.test_commit_needing(tools.test_fold_root(), ARRAY['extension test_prolly_ordering'],
 ARRAY['{1.1,public,ordering}']) AS exact_new \gset
SELECT tools.test_commit_needing(tools.test_fold_root(), ARRAY['extension test_prolly_ordering'],
 ARRAY['{1.0,public}']) AS minimum_old \gset
SELECT tools.test_check_dependencies(ARRAY[:'exact_new', :'minimum_old'], true);
SELECT extversion FROM pg_extension WHERE extname = 'test_prolly_ordering';

-- Both flags survive when the extension itself lives in a versioned schema.
\c :db
CREATE DATABASE regress_ordering_versioned;
\c regress_ordering_versioned
CREATE SCHEMA tools;
CREATE EXTENSION test_prolly SCHEMA tools;
CREATE SCHEMA ord VERSIONED;
CREATE EXTENSION test_prolly_ordering SCHEMA ord VERSION '1.0';
CREATE TABLE ord.t (k ord.test_ordering PRIMARY KEY);
INSERT INTO ord.t VALUES (ROW(1));
SELECT length(dolt_commit('-Am', 'versioned extension')::text) AS committed;
SELECT replace(d, E'\t', ' | ') AS dependency
 FROM unnest(tools.test_commit_dependencies(dolt_hashof('HEAD'))) d ORDER BY 1;
SELECT tools.test_check_dependencies(ARRAY[dolt_hashof('HEAD')], false);
SELECT dolt_remote('add', 'origin', :'url' || '-versioned');
SELECT dolt_push('origin', 'main');
\c :db
CREATE DATABASE regress_ordering_versioned_clone;
\c regress_ordering_versioned_clone
SELECT dolt_clone(:'url' || '-versioned');
SELECT extversion, extnamespace::regnamespace FROM pg_extension WHERE extname = 'test_prolly_ordering';
SELECT * FROM ord.t;

\c :db
DROP DATABASE regress_ordering_versioned_clone;
DROP DATABASE regress_ordering_versioned;
DROP DATABASE regress_ordering_conflict;
DROP DATABASE regress_ordering_wrong;
DROP DATABASE regress_ordering_dst;
DROP DATABASE regress_ordering_src;
