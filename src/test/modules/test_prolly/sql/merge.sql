-- Three-way merges of branches made by copying: each side's changes since
-- their common ancestor, conflicts where both changed a row differently.

CREATE SCHEMA app VERSIONED;
CREATE TABLE app.t (id int PRIMARY KEY, v text);
CREATE TABLE app.k (a text);
CREATE TABLE app.untouched (x int PRIMARY KEY);
INSERT INTO app.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
INSERT INTO app.k VALUES ('x'), ('y'), ('y');
INSERT INTO app.untouched VALUES (1);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');

-- main and feature change different rows, and one row alike
UPDATE app.t SET v = 'main' WHERE id IN (1, 2);
DELETE FROM app.t WHERE id = 3;
INSERT INTO app.t VALUES (301, 'main');
UPDATE app.t SET v = 'alike' WHERE id = 100;
INSERT INTO app.k VALUES ('y');
SELECT length(dolt_commit('-Am', 'on main')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE app.t SET v = 'feature' WHERE id IN (10, 20);
DELETE FROM app.t WHERE id = 30;
INSERT INTO app.t VALUES (302, 'feature');
UPDATE app.t SET v = 'alike' WHERE id = 100;
DELETE FROM app.k WHERE ctid = (SELECT min(ctid) FROM app.k WHERE a = 'y');
INSERT INTO app.k VALUES ('z');
SELECT length(dolt_commit('-Am', 'on feature')::text) AS committed;
SELECT dolt_checkout('main');

SELECT (dolt_merge('feature'))[2:4];
SELECT string_agg(message, ', ' ORDER BY message COLLATE "C") FROM
  (SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 4) s;
SELECT dolt_hashof('HEAD^1') = (SELECT commit_hash FROM dolt_log('main') WHERE message = 'on main') AS first_parent,
       dolt_hashof('HEAD^2') = dolt_hashof('feature') AS second_parent;
SELECT id, v FROM app.t WHERE id IN (1, 2, 3, 10, 20, 30, 100, 301, 302) ORDER BY id;
SELECT count(*) FROM app.t;
SELECT a, count(*) FROM app.k GROUP BY a ORDER BY a;
SELECT * FROM dolt_status();
SELECT (dolt_merge('feature'))[2:4];		-- merged already

-- Conflicts: both change row 5, differently; feature also changes row 6.
SELECT dolt_checkout('feature');
SELECT (dolt_merge('main'))[2:4];			-- feature fast-forwards
UPDATE app.t SET v = 'feature 5' WHERE id = 5;
UPDATE app.t SET v = 'feature 6' WHERE id = 6;
SELECT length(dolt_commit('-Am', 'feature 5')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main 5' WHERE id = 5;
SELECT length(dolt_commit('-Am', 'main 5')::text) AS committed;

-- outside a transaction block, the merge fails and changes nothing
SELECT dolt_merge('feature');
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;
SELECT * FROM dolt_conflicts();

BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM dolt_status();
SELECT * FROM dolt_conflicts();
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::app.t);
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;	-- ours kept; theirs merged
SAVEPOINT s;
SELECT dolt_commit('-Am', 'too soon');
ROLLBACK TO s;
COMMIT;
-- (the transaction's commit was refused, so nothing of it stayed)
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;

-- Resolved with theirs, then committed: a merge commit.
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_conflicts_resolve('--theirs', 'app.t');
SELECT * FROM dolt_conflicts();
SELECT v FROM app.t WHERE id IN (5, 6) ORDER BY id;
SELECT length(dolt_commit('-Am', 'resolved')::text) AS committed;
COMMIT;
SELECT dolt_hashof('HEAD^2') = dolt_hashof('feature') AS merge_commit;
SELECT * FROM dolt_status();

-- Resolved with ours, then aborted.
SELECT dolt_checkout('feature');
UPDATE app.t SET v = 'feature again' WHERE id = 7;
SELECT length(dolt_commit('-Am', 'feature 7')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main again' WHERE id = 7;
SELECT length(dolt_commit('-Am', 'main 7')::text) AS committed;
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT dolt_conflicts_resolve('--ours', '.');
SELECT v FROM app.t WHERE id = 7;
SELECT (dolt_merge('--abort'))[2:4];
SELECT v FROM app.t WHERE id = 7;
SELECT * FROM dolt_conflicts();
COMMIT;
-- with dolt.allow_commit_conflicts they can be kept
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM dolt_conflicts();
SELECT dolt_merge('feature');		-- one at a time
SELECT (dolt_merge('--abort'))[2:4];
RESET dolt.allow_commit_conflicts;

-- A keyless table's copies: a row has the base's, those either side added,
-- less the most either removed, and never conflicts.  Here main removes
-- every y and z, feature a y and the z, and adds a w: no y, no z, a w.
SELECT dolt_branch('-D', 'feature');
SELECT dolt_branch('feature');
DELETE FROM app.k WHERE a IN ('y', 'z');
SELECT length(dolt_commit('-Am', 'main drops y and z')::text) AS committed;
SELECT dolt_checkout('feature');
DELETE FROM app.k WHERE ctid = (SELECT min(ctid) FROM app.k WHERE a = 'y');
DELETE FROM app.k WHERE a = 'z';
INSERT INTO app.k VALUES ('w');
SELECT length(dolt_commit('-Am', 'feature drops a y and z')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT a, count(*) FROM app.k GROUP BY a ORDER BY a;
ROLLBACK;

-- Definitions that differ: merged, then the rows (app.k's as above).
SELECT dolt_checkout('feature');
ALTER TABLE app.t ADD COLUMN w int;
SELECT length(dolt_commit('-Am', 'a column')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE app.t SET v = 'main 8' WHERE id = 8;
SELECT length(dolt_commit('-Am', 'main 8')::text) AS committed;
SELECT (dolt_merge('feature'))[2:4];

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA app CASCADE;

-- Keyless rows changed on both sides, case by case: the same change on
-- both is made once (the same update, a pick the branch has); otherwise
-- copies added on each are each a row, copies removed on both are removed
-- once.  None conflicts (each merge's own conflicts are shown).
CREATE SCHEMA mk VERSIONED;
CREATE TABLE mk.t (a int, b text);
CREATE FUNCTION pg_temp.merged(base text[], ours text, theirs text,
	OUT a int, OUT b text, OUT copies int8, OUT conflicts text) RETURNS SETOF record AS $$
DECLARE
	m text[];
BEGIN
	TRUNCATE mk.t;
	EXECUTE 'INSERT INTO mk.t VALUES ' || array_to_string(base, ', ');
	PERFORM dolt_commit('-Am', 'base');
	PERFORM dolt_branch('mk_side');
	PERFORM dolt_checkout('mk_side');
	EXECUTE theirs;
	PERFORM dolt_commit('-am', 'theirs');
	PERFORM dolt_checkout('main');
	EXECUTE ours;
	PERFORM dolt_commit('--allow-empty', '-am', 'ours');
	m := dolt_merge('mk_side');
	PERFORM dolt_branch('-D', 'mk_side');
	RETURN QUERY SELECT t.a, t.b, count(*), m[3] FROM mk.t t GROUP BY t.a, t.b ORDER BY 1, 2;
END $$ LANGUAGE plpgsql;
SELECT 'both add a copy' AS "case", * FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$],
	$s$INSERT INTO mk.t VALUES (1, 'x')$s$, $s$INSERT INTO mk.t VALUES (1, 'x')$s$);
SELECT 'both add the same new row' AS "case", * FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$],
	$s$INSERT INTO mk.t VALUES (2, 'y')$s$, $s$INSERT INTO mk.t VALUES (2, 'y')$s$);
SELECT 'each adds a different row' AS "case", * FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$],
	$s$INSERT INTO mk.t VALUES (2, 'y')$s$, $s$INSERT INTO mk.t VALUES (3, 'z')$s$);
SELECT 'both delete it' AS "case", * FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$, $s$(2, 'y')$s$],
	$s$DELETE FROM mk.t WHERE a = 1$s$, $s$DELETE FROM mk.t WHERE a = 1$s$);
SELECT 'both update it differently' AS "case", * FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$],
	$s$UPDATE mk.t SET b = 'ours'$s$, $s$UPDATE mk.t SET b = 'theirs'$s$);
SELECT 'one deletes a copy of two, one adds one' AS "case", *
  FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$, $s$(1, 'x')$s$],
	$s$DELETE FROM mk.t WHERE ctid = (SELECT min(ctid) FROM mk.t)$s$,
	$s$INSERT INTO mk.t VALUES (1, 'x')$s$);
SELECT 'both delete a copy of three' AS "case", *
  FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$, $s$(1, 'x')$s$, $s$(1, 'x')$s$],
	$s$DELETE FROM mk.t WHERE ctid = (SELECT min(ctid) FROM mk.t)$s$,
	$s$DELETE FROM mk.t WHERE ctid = (SELECT max(ctid) FROM mk.t)$s$);
SELECT 'both update it alike' AS "case", * FROM pg_temp.merged(ARRAY[$s$(1, 'x')$s$],
	$s$UPDATE mk.t SET b = 'y'$s$, $s$UPDATE mk.t SET b = 'y'$s$);
-- a hotfix picked onto main, then its branch merged: made once
TRUNCATE mk.t;
INSERT INTO mk.t VALUES (1, 'x');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('mk_side');
SELECT dolt_checkout('mk_side') IS NOT NULL AS on_side;
INSERT INTO mk.t VALUES (9, 'hotfix');
SELECT length(dolt_commit('-am', 'hotfix')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT (dolt_cherry_pick('mk_side'))[2:4];
INSERT INTO mk.t VALUES (2, 'ours');
SELECT length(dolt_commit('-am', 'ours')::text) AS committed;
SELECT (dolt_merge('mk_side'))[2:4];
SELECT a, b, count(*) AS copies FROM mk.t GROUP BY a, b ORDER BY a;
SELECT dolt_branch('-D', 'mk_side');
DROP SCHEMA mk CASCADE;
SELECT length(dolt_commit('-Am', 'mk gone')::text) AS committed;

-- A merge reads rows as they were folded: a NULL in a column added with a
-- default is NULL in the conflict's rows, not the default.
CREATE SCHEMA mgd VERSIONED;
CREATE TABLE mgd.t (id int PRIMARY KEY, a text);
INSERT INTO mgd.t VALUES (1, 'one');
ALTER TABLE mgd.t ADD COLUMN c int DEFAULT 5;
SELECT length(dolt_commit('-Am', 'c is 5')::text) AS committed;
SELECT dolt_branch('mgd_side');
SELECT dolt_checkout('mgd_side') IS NOT NULL AS on_side;
UPDATE mgd.t SET c = NULL;
SELECT length(dolt_commit('-am', 'c is NULL')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE mgd.t SET a = 'uno';
SELECT length(dolt_commit('-am', 'a is uno')::text) AS committed;
BEGIN;
SELECT (dolt_merge('mgd_side'))[2:4];
SELECT our_row, their_row FROM dolt_conflicts(NULL::mgd.t);
ROLLBACK;
SELECT dolt_branch('-D', 'mgd_side');
DROP SCHEMA mgd CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- A merge doesn't read the rows of a table whose collation differs now
-- from what a version recorded.
CREATE SCHEMA mgc VERSIONED;
CREATE COLLATION mgc.c_utf8 (provider = builtin, locale = 'C.UTF-8');
CREATE TABLE mgc.t (k text COLLATE mgc.c_utf8 PRIMARY KEY, v int);
INSERT INTO mgc.t VALUES ('a', 1), ('b', 1);
SELECT length(dolt_commit('-Am', 'collated')::text) AS committed;
SELECT dolt_branch('mgc_side');
SELECT dolt_checkout('mgc_side') IS NOT NULL AS on_side;
UPDATE mgc.t SET v = 2 WHERE k = 'a';
SELECT length(dolt_commit('-am', 'a is 2')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE mgc.t SET v = 3 WHERE k = 'b';
SELECT length(dolt_commit('-am', 'b is 3')::text) AS committed;
UPDATE pg_collation SET collicurules = '&a<b' WHERE collname = 'c_utf8';
SELECT (dolt_merge('mgc_side'))[2:4];
UPDATE pg_collation SET collicurules = NULL WHERE collname = 'c_utf8';
SELECT (dolt_merge('mgc_side'))[2:4];
SELECT * FROM mgc.t ORDER BY k;
SELECT dolt_branch('-D', 'mgc_side');
DROP SCHEMA mgc CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- Theirs swapping two rows' unique values merges: the rows going are
-- removed before those coming are written.
CREATE SCHEMA mgu VERSIONED;
CREATE TABLE mgu.t (id int PRIMARY KEY, u int UNIQUE, v text);
INSERT INTO mgu.t VALUES (1, 1, 'a'), (2, 2, 'b'), (3, 3, 'c');
SELECT length(dolt_commit('-Am', 'unique')::text) AS committed;
SELECT dolt_branch('mgu_side');
SELECT dolt_checkout('mgu_side') IS NOT NULL AS on_side;
UPDATE mgu.t SET u = 10 WHERE id = 1;
UPDATE mgu.t SET u = 1 WHERE id = 2;
UPDATE mgu.t SET u = 2 WHERE id = 1;
SELECT length(dolt_commit('-am', 'swapped')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE mgu.t SET v = 'main' WHERE id = 3;
SELECT length(dolt_commit('-am', 'main too')::text) AS committed;
SELECT (dolt_merge('mgu_side'))[2:4];
SELECT * FROM mgu.t ORDER BY id;

-- A merge in progress keeps the commits it merges, though their branch
-- goes; and goes with its own branch, not passing to one made later.
SELECT dolt_branch('mgu_k');
SELECT dolt_checkout('mgu_k') IS NOT NULL AS on_k;
UPDATE mgu.t SET v = 'k' WHERE id = 3;
SELECT length(dolt_commit('-am', 'k')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('mgu_m');
SELECT dolt_checkout('mgu_m') IS NOT NULL AS on_m;
UPDATE mgu.t SET v = 'm' WHERE id = 3;
SELECT length(dolt_commit('-am', 'm')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('mgu_k'))[2:4];
RESET dolt.allow_commit_conflicts;
SELECT dolt_hashof('mgu_k') AS theirs \gset
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'mgu_k');
SELECT test_refs_walk_reaches(:'theirs') AS kept;
SELECT dolt_branch('-D', 'mgu_m');
SELECT test_refs_walk_reaches(:'theirs') AS kept_once_its_merge_is_gone;
SELECT dolt_branch('mgu_m');
SELECT dolt_checkout('mgu_m') IS NOT NULL AS on_m;
SELECT * FROM dolt_conflicts();
SELECT dolt_merge('--abort');
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', 'mgu_m');

-- Unresolved conflicts aren't committed by checking out another branch
-- first, nor by preparing the transaction.
SELECT dolt_branch('mgu_k');
SELECT dolt_checkout('mgu_k') IS NOT NULL AS on_k;
UPDATE mgu.t SET v = 'k' WHERE id = 3;
SELECT length(dolt_commit('-am', 'k')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
UPDATE mgu.t SET v = 'main again' WHERE id = 3;
INSERT INTO mgu.t VALUES (4, 4, 'main');
SELECT length(dolt_commit('-am', 'main again')::text) AS committed;
SELECT dolt_checkout('mgu_k') IS NOT NULL AS on_k;
INSERT INTO mgu.t VALUES (5, 5, 'k');
SELECT length(dolt_commit('-am', 'k adds 5')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('mgu_k'))[2:4];
SELECT dolt_checkout('mgu_k') IS NOT NULL AS on_k;
COMMIT;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SELECT (dolt_merge('mgu_k'))[2:4];
PREPARE TRANSACTION 'mgu';
SELECT * FROM dolt_conflicts();

-- A merge that wrote rows (5, here) is aborted in its transaction: the
-- branch is as it was.
BEGIN;
SELECT (dolt_merge('mgu_k'))[2:4];
SELECT id, v FROM mgu.t ORDER BY id;
SELECT (dolt_merge('--abort'))[2:4];
SELECT id, v FROM mgu.t ORDER BY id;
COMMIT;
SELECT * FROM dolt_status();
SELECT dolt_branch('-D', 'mgu_k');
SELECT dolt_branch('-D', 'mgu_side');
DROP SCHEMA mgu CASCADE;
SELECT length(dolt_commit('-Am', 'without it')::text) AS committed;

-- A history of merges reaches its commits along many paths (each round
-- here doubles them); finding a merge base visits each commit once.
CREATE SCHEMA mb VERSIONED;
CREATE TABLE mb.t (id int PRIMARY KEY, v int);
INSERT INTO mb.t VALUES (0, 0);
SELECT length(dolt_commit('-Am', 'mb base')::text) AS committed;
SELECT dolt_branch('mb_side');
SET statement_timeout = '60s';
DO $$
BEGIN
	FOR i IN 1..24 LOOP
		PERFORM dolt_checkout('mb_side');
		PERFORM dolt_merge('main');
		INSERT INTO mb.t VALUES (2 * i, i);
		PERFORM dolt_commit('-am', 'side ' || i);
		PERFORM dolt_checkout('main');
		INSERT INTO mb.t VALUES (2 * i + 1, i);
		PERFORM dolt_commit('-am', 'main ' || i);
		PERFORM dolt_merge('mb_side', '-m', 'merge ' || i);
	END LOOP;
END $$;
RESET statement_timeout;
SELECT count(*) AS rows FROM mb.t;
SELECT count(*) AS merges FROM dolt_log('main') WHERE message LIKE 'merge %';
SELECT dolt_branch('-D', 'mb_side');
DROP SCHEMA mb CASCADE;
SELECT length(dolt_commit('-Am', 'without mb')::text) AS committed;
