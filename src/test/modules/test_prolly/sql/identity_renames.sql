-- Tables and columns keep their identity through renames: a commit records
-- it beside its root (never in it), and historical reads, diffs and merges
-- match objects by it, so a rename is not a drop and an add.
CREATE DATABASE regress_identity_renames;
\c regress_identity_renames
CREATE EXTENSION test_prolly;
CREATE SCHEMA app VERSIONED;
CREATE SCHEMA other VERSIONED;

-- A rename on one branch, an update of the renamed column on another.
CREATE TABLE app.users (id int PRIMARY KEY, email text);
INSERT INTO app.users VALUES (42, 'person42');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT test_commit_identity('HEAD') IS NULL AS nothing_recorded;
ALTER TABLE app.users RENAME COLUMN email TO address;
-- (uncommitted: the head's rows read as the table is named now)
SELECT * FROM dolt_at('HEAD', NULL::app.users);
SELECT replace(test_working_identity(), E'\t', ' ') AS identity;
SELECT length(dolt_commit('-Am', 'rename email to address')::text) AS committed;
SELECT replace(test_commit_identity('HEAD'), E'\t', ' ') AS identity;
SELECT * FROM dolt_at('HEAD~1', NULL::app.users);
SELECT dolt_checkout('-b', 'edit', 'HEAD~1');
UPDATE app.users SET email = 'changed' WHERE id = 42;
SELECT length(dolt_commit('-Am', 'change email')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('edit'))[3] AS conflicts;
SELECT * FROM app.users;
SELECT * FROM dolt_status;
SELECT replace(test_commit_identity('HEAD'), E'\t', ' ') AS identity;

-- The other direction: theirs renames (and edits another row), ours edits.
CREATE TABLE app.dir (id int PRIMARY KEY, email text, n int);
INSERT INTO app.dir VALUES (1, 'a', 10), (2, 'b', 20);
SELECT length(dolt_commit('-Am', 'dir base')::text) AS committed;
SELECT dolt_branch('renamer');
UPDATE app.dir SET email = 'a2' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'ours edit')::text) AS committed;
SELECT dolt_checkout('renamer');
ALTER TABLE app.dir RENAME COLUMN email TO address;
UPDATE app.dir SET address = 'b2' WHERE id = 2;
SELECT length(dolt_commit('-Am', 'theirs rename')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('renamer'))[3] AS conflicts;
SELECT * FROM app.dir ORDER BY id;
SELECT * FROM dolt_status;

-- Historical reads across the rename: each version's stored values.
SELECT * FROM dolt_at('HEAD~1', NULL::app.dir) ORDER BY id;
SELECT (to_row).id, (to_row).address AS to_address, (from_row).address AS from_address,
	   diff_type
FROM dolt_diff_table(NULL::app.dir, 'HEAD~2', 'HEAD') ORDER BY 1, to_commit;
SELECT (row).id, (row).address FROM dolt_history_table(NULL::app.dir)
WHERE commit_hash = dolt_hashof('HEAD~1') ORDER BY 1;

-- The old name reused: a new column, not the renamed one.
ALTER TABLE app.dir ADD COLUMN email text DEFAULT 'new';
UPDATE app.dir SET email = 'fresh' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'email again')::text) AS committed;
SELECT replace(test_commit_identity('HEAD'), E'\t', ' ') AS identity;
SELECT * FROM dolt_at('HEAD~2', NULL::app.dir) ORDER BY id;
-- Names swapped: each column's values go with it.
ALTER TABLE app.dir RENAME COLUMN email TO tmp;
ALTER TABLE app.dir RENAME COLUMN address TO email;
ALTER TABLE app.dir RENAME COLUMN tmp TO address;
SELECT * FROM dolt_at('HEAD', NULL::app.dir) ORDER BY id;
SELECT length(dolt_commit('-Am', 'swap')::text) AS committed;
SELECT * FROM dolt_at('HEAD~1', NULL::app.dir) ORDER BY id;
SELECT * FROM dolt_at('HEAD~3', NULL::app.dir) ORDER BY id;

-- A key column renamed (composite key), and the same row's cells changed on
-- both sides, one of them renaming another column twice.
CREATE TABLE app.k (a int, b int, v text, w int, PRIMARY KEY (a, b));
INSERT INTO app.k VALUES (1, 1, 'x', 1), (1, 2, 'y', 2);
SELECT length(dolt_commit('-Am', 'k base')::text) AS committed;
SELECT dolt_branch('k_theirs');
SELECT dolt_branch('k_conflict');
SELECT dolt_branch('k_same');
ALTER TABLE app.k RENAME COLUMN b TO bb;
UPDATE app.k SET w = 100 WHERE a = 1 AND bb = 1;
SELECT length(dolt_commit('-Am', 'k ours')::text) AS committed;
SELECT dolt_checkout('k_theirs');
ALTER TABLE app.k RENAME COLUMN v TO v1;
SELECT length(dolt_commit('-Am', 'k theirs 1')::text) AS committed;
ALTER TABLE app.k RENAME COLUMN v1 TO v2;
UPDATE app.k SET v2 = 'xx' WHERE a = 1 AND b = 1;
SELECT length(dolt_commit('-Am', 'k theirs 2')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('k_theirs'))[3] AS conflicts;
SELECT * FROM app.k ORDER BY a, bb;
SELECT * FROM dolt_at('k_theirs~1', NULL::app.k) ORDER BY a, bb;
SELECT * FROM dolt_at('HEAD~1', NULL::app.k) ORDER BY a, bb;
-- renamed otherwise on each side: a schema conflict
SELECT dolt_checkout('k_conflict');
ALTER TABLE app.k RENAME COLUMN v TO vthird;
SELECT length(dolt_commit('-Am', 'k conflict')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('k_conflict');
-- renamed alike on both sides: no conflict
SELECT dolt_checkout('k_same');
ALTER TABLE app.k RENAME COLUMN b TO bb;
UPDATE app.k SET v = 'yy' WHERE bb = 2;
SELECT length(dolt_commit('-Am', 'k same')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('k_same'))[3] AS conflicts;
SELECT * FROM app.k ORDER BY a, bb;

-- A conflict on a renamed column's cell, listed and resolved as named now.
CREATE TABLE app.c (id int PRIMARY KEY, v text);
INSERT INTO app.c VALUES (1, 'base');
SELECT length(dolt_commit('-Am', 'c base')::text) AS committed;
SELECT dolt_branch('c_theirs');
ALTER TABLE app.c RENAME COLUMN v TO val;
UPDATE app.c SET val = 'ours';
SELECT length(dolt_commit('-Am', 'c ours')::text) AS committed;
SELECT dolt_checkout('c_theirs');
UPDATE app.c SET v = 'theirs';
SELECT length(dolt_commit('-Am', 'c theirs')::text) AS committed;
SELECT dolt_checkout('main');
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('c_theirs'))[3] AS conflicts;
SELECT (base_row).val AS base, (our_row).val AS ours, (their_row).val AS theirs
FROM dolt_conflicts(NULL::app.c);
SELECT dolt_conflicts_resolve('--theirs', 'app.c');
SELECT * FROM app.c;
SELECT length(dolt_commit('-Am', 'c merged')::text) AS committed;
RESET dolt.allow_commit_conflicts;

-- One side drops a column and renames another to its name.
CREATE TABLE app.d (id int PRIMARY KEY, a text, c text);
INSERT INTO app.d VALUES (1, 'a1', 'c1');
SELECT length(dolt_commit('-Am', 'd base')::text) AS committed;
SELECT dolt_branch('d_theirs');
ALTER TABLE app.d DROP COLUMN c;
ALTER TABLE app.d RENAME COLUMN a TO c;
SELECT length(dolt_commit('-Am', 'd ours')::text) AS committed;
SELECT dolt_checkout('d_theirs');
INSERT INTO app.d VALUES (2, 'a2', 'c2');
SELECT length(dolt_commit('-Am', 'd theirs')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('d_theirs'))[3] AS conflicts;
SELECT * FROM app.d ORDER BY id;

-- A column dropped and one of its name added has the dropped one's
-- identity (as Dolt's tags have it); a rename keeps the column's own.
CREATE TABLE app.r (id int PRIMARY KEY, w int);
INSERT INTO app.r VALUES (1, 7);
SELECT length(dolt_commit('-Am', 'r base')::text) AS committed;
ALTER TABLE app.r DROP COLUMN w;
ALTER TABLE app.r ADD COLUMN w int;
SELECT * FROM dolt_at('HEAD', NULL::app.r);
SELECT position(E'app.r\t' IN test_working_identity()) = 0 AS own_names;

-- Tables: renamed with their rows changed, moved to another schema, the old
-- name reused by an unrelated table; and merged with rows changed elsewhere.
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 't base')::text) AS committed;
SELECT dolt_branch('t_theirs');
ALTER TABLE app.t RENAME TO renamed_t;
UPDATE app.renamed_t SET v = 'uno' WHERE id = 1;
SELECT length(dolt_commit('-Am', 't renamed and changed')::text) AS committed;
ALTER TABLE app.renamed_t SET SCHEMA other;
SELECT length(dolt_commit('-Am', 't moved')::text) AS committed;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
INSERT INTO app.t VALUES (9, 'nine');
SELECT length(dolt_commit('-Am', 'new t')::text) AS committed;
SELECT dolt_checkout('t_theirs');
UPDATE app.t SET v = 'dos' WHERE id = 2;
SELECT length(dolt_commit('-Am', 't theirs')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('t_theirs'))[3] AS conflicts;
SELECT * FROM other.renamed_t ORDER BY id;
SELECT * FROM app.t ORDER BY id;
SELECT * FROM dolt_status;

-- A pick of a change to a column renamed since; then a reset to before the
-- rename, after which a rename is still told from a drop and an add.
CREATE TABLE app.p (id int PRIMARY KEY, v text);
INSERT INTO app.p VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'p base')::text) AS committed;
SELECT dolt_branch('p_theirs');
ALTER TABLE app.p RENAME COLUMN v TO val;
SELECT length(dolt_commit('-Am', 'p renamed')::text) AS committed;
SELECT dolt_checkout('p_theirs');
UPDATE app.p SET v = 'uno';
SELECT length(dolt_commit('-Am', 'p theirs')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_cherry_pick('p_theirs'))[2:4];
SELECT * FROM app.p;
SELECT dolt_reset('--hard', 'HEAD~2');
SELECT * FROM app.p;
ALTER TABLE app.p RENAME COLUMN v TO renamed_again;
SELECT * FROM dolt_at('HEAD', NULL::app.p);
SELECT dolt_reset('--hard');

-- The same tables and rows reached by other renames are the same root;
-- only the identities their commits record differ.
SELECT dolt_checkout('-b', 'plain', 'HEAD');
CREATE TABLE app.same (id int PRIMARY KEY, name text);
INSERT INTO app.same VALUES (1, 'x');
SELECT length(dolt_commit('-Am', 'made as named')::text) AS committed;
SELECT dolt_checkout('-b', 'renamed', 'HEAD~1');
CREATE TABLE app.made (id int PRIMARY KEY, label text);
INSERT INTO app.made VALUES (1, 'x');
SELECT length(dolt_commit('-Am', 'made otherwise')::text) AS committed;
ALTER TABLE app.made RENAME COLUMN label TO name;
ALTER TABLE app.made RENAME TO same;
-- (its constraints named as made as named: names are what a root holds)
ALTER INDEX app.made_pkey RENAME TO same_pkey;
ALTER TABLE app.same RENAME CONSTRAINT made_id_not_null TO same_id_not_null;
SELECT length(dolt_commit('-Am', 'renamed to it')::text) AS committed;
SELECT prolly_root_hash('plain') = prolly_root_hash('renamed') AS same_root;
SELECT test_commit_identity('plain') IS DISTINCT FROM test_commit_identity('renamed')
	AS identities_differ;
SELECT test_commit_identity('renamed') LIKE '%app.made%' AS renamed_recorded;
SELECT dolt_checkout('main');

\c contrib_regression
DROP DATABASE regress_identity_renames;
