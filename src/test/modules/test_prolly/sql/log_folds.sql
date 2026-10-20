-- incremental_folds's cases folded by the change finder "log": a commit
-- folds only what changed since the branch's last commit, the rows its
-- tables' change logs name.  The tree is always the one a fold of every
-- row gives, and each row changed is one candidate.  (The pages column
-- counts the page versions "pages" reads: "log" reads none, so it is
-- always 0 here; the candidates are what say how little was read.)
SET debug_prolly_change_finder = log;

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA inc VERSIONED;
CREATE TABLE inc.big (id int PRIMARY KEY, v text, n int);
CREATE INDEX big_n ON inc.big (n);
CREATE TABLE inc.keyless (a int, b text);
INSERT INTO inc.big SELECT g, 'v' || g, g FROM generate_series(1, 20000) g;
INSERT INTO inc.keyless SELECT g % 100, 'k' FROM generate_series(1, 1000) g;
SELECT pg_relation_size('inc.big') / 8192 > 100 AS many_pages;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- fold(): fold the working set from the baseline, say what it read, and
-- check it against a fold of every row.
CREATE FUNCTION pg_temp.fold(OUT pages int8, OUT candidates int8,
	OUT full_tables int8, OUT same_as_full bool) AS $$
DECLARE
	inc text;
BEGIN
	PERFORM test_fold_counters(true);
	inc := test_fold_root();
	SELECT c.pages, c.candidates, c.full_tables
	  INTO pages, candidates, full_tables FROM test_fold_counters(true) c;
	same_as_full := inc = test_fold_root(true);
END $$ LANGUAGE plpgsql;

-- Nothing changed: nothing read.
SELECT * FROM pg_temp.fold();

-- A HOT update, an update of an indexed column, a delete and an insert.
UPDATE inc.big SET v = 'hot' WHERE id = 10;
UPDATE inc.big SET n = -n WHERE id = 15000;
DELETE FROM inc.big WHERE id = 7;
INSERT INTO inc.big VALUES (20001, 'new', 1);
SELECT * FROM pg_temp.fold();
SELECT table_name, status FROM dolt_status ORDER BY 1;
SELECT diff_type, (from_row).id AS from_id, (to_row).id AS to_id
FROM dolt_diff('HEAD', 'WORKING', NULL::inc.big) ORDER BY coalesce((to_row).id, (from_row).id);

-- The commit's fold is the new baseline.
SELECT length(dolt_commit('-am', 'changes')::text) AS committed;
SELECT * FROM pg_temp.fold();
SELECT count(*) AS clean FROM dolt_status;

-- Pruning and freezing change no row: they list nothing.
VACUUM (FREEZE) inc.big;
SELECT * FROM pg_temp.fold();

-- A commit rolled back leaves the baseline as it was.
BEGIN;
UPDATE inc.big SET v = 'rolled back' WHERE id = 1;
SELECT length(dolt_commit('-am', 'gone')::text) AS committed;
ROLLBACK;
UPDATE inc.big SET v = 'kept' WHERE id = 2;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'kept')::text) AS committed;

-- A truncation, and the pages added after it.
DELETE FROM inc.big WHERE id > 19000;
VACUUM inc.big;
INSERT INTO inc.big SELECT g, 'again', g FROM generate_series(19001, 19100) g;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'tail')::text) AS committed;

-- A table without a primary key keys a row by its content and counts its
-- copies (each row here has ten): each copy made or replaced is an entry,
-- counted against the baseline's count of the row, not read in full.
UPDATE inc.keyless SET b = 'changed' WHERE a = 5;
SELECT * FROM pg_temp.fold();
-- one copy of ten deleted, one more added, a new row, and an aborted
-- insert and delete
DELETE FROM inc.keyless
 WHERE ctid = (SELECT min(ctid) FROM inc.keyless WHERE a = 6);
INSERT INTO inc.keyless VALUES (7, 'k'), (1000, 'new');
BEGIN;
INSERT INTO inc.keyless VALUES (8, 'aborted');
DELETE FROM inc.keyless WHERE a = 9;
ROLLBACK;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless')::text) AS committed;
SELECT * FROM pg_temp.fold();
-- every copy of a row deleted, and the table's last pages emptied and
-- truncated (vacuum's truncation changes no row: no entry)
DELETE FROM inc.keyless WHERE a = 10;
DELETE FROM inc.keyless WHERE ctid >= '(4,0)';
VACUUM inc.keyless;
SELECT pg_relation_size('inc.keyless') / 8192 AS keyless_pages;
SELECT * FROM pg_temp.fold();
-- in the transaction writing: its own writes are counted, and its
-- commit's fold consumes their entries (they come back if it rolls back),
-- so the next fold reads only the entries written since; a second fold
-- in the transaction too
BEGIN;
UPDATE inc.keyless SET b = 'mine' WHERE a = 11;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless again')::text) AS committed;
UPDATE inc.keyless SET b = 'mine too' WHERE a = 14;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless twice')::text) AS committed;
COMMIT;
UPDATE inc.keyless SET b = 'after' WHERE a = 12;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless after')::text) AS committed;
UPDATE inc.keyless SET b = 'later' WHERE a = 13;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless later')::text) AS committed;
-- a commit rolled back with its transaction: the baseline before it holds
BEGIN;
UPDATE inc.keyless SET b = 'rolled back' WHERE a = 15;
SELECT length(dolt_commit('-am', 'rolled back')::text) AS committed;
ROLLBACK;
UPDATE inc.keyless SET b = 'kept' WHERE a = 16;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'kept')::text) AS committed;
-- a commit in a savepoint rolled back: the baseline it made goes with it,
-- and the writes it saw as settled with them
BEGIN;
SAVEPOINT s;
UPDATE inc.keyless SET b = 'in the savepoint' WHERE a = 19;
UPDATE inc.big SET v = 'in the savepoint' WHERE id BETWEEN 2000 AND 2099;
SELECT length(dolt_commit('-am', 'in the savepoint')::text) AS committed;
ROLLBACK TO s;
UPDATE inc.keyless SET b = 'after the savepoint' WHERE a = 20;
UPDATE inc.big SET v = 'after the savepoint' WHERE id BETWEEN 2100 AND 2199;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'after the savepoint')::text) AS committed;
COMMIT;
UPDATE inc.keyless SET b = 'later' WHERE a = 21;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'later still')::text) AS committed;
SELECT count(*) AS rows_from_the_savepoint FROM inc.keyless WHERE b = 'in the savepoint';
-- a cherry-pick commits in the transaction applying its rows: the next
-- fold reads only the entries written since, keyless and keyed
SELECT dolt_branch('pick');
SELECT dolt_checkout('pick') IS NOT NULL AS on_pick;
UPDATE inc.keyless SET b = 'picked' WHERE a = 17;
UPDATE inc.big SET v = 'picked' WHERE id BETWEEN 100 AND 1099;
SELECT length(dolt_commit('-am', 'to pick')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT length(dolt_cherry_pick('pick')::text) > 0 AS picked;
UPDATE inc.keyless SET b = 'after the pick' WHERE a = 18;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'after the pick')::text) AS committed;
SELECT dolt_branch('-D', 'pick');

-- A commit rolled back between two deletes on one page: the entries its
-- fold consumed come back with the rollback, and each delete is counted
-- once.  (For "pages", the case of a page listed in two epochs.)
CREATE FUNCTION pg_temp.on_page(blk int, first bool) RETURNS tid AS $$
	SELECT CASE WHEN first THEN min(ctid) ELSE max(ctid) END FROM inc.keyless
	 WHERE ctid >= format('(%s,0)', blk)::tid AND ctid < format('(%s,0)', blk + 1)::tid
$$ LANGUAGE sql;
DELETE FROM inc.keyless WHERE ctid = (SELECT pg_temp.on_page(1, true));
BEGIN;
SELECT length(dolt_commit('-am', 'rolled back')::text) AS committed;
ROLLBACK;
DELETE FROM inc.keyless WHERE ctid = (SELECT pg_temp.on_page(1, false));
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'two epochs')::text) AS committed;

-- A row an update deleted before the baseline, its xmax a multixact (a
-- key share lock, then the update in a subtransaction): its old version
-- is told apart without reading the table in full.
BEGIN;
SELECT a FROM inc.keyless WHERE ctid = (SELECT pg_temp.on_page(2, true)) FOR KEY SHARE;
SAVEPOINT s;
UPDATE inc.keyless SET b = 'multixact' WHERE ctid = (SELECT pg_temp.on_page(2, true));
RELEASE s;
COMMIT;
SELECT length(dolt_commit('-am', 'multixact')::text) AS committed;
UPDATE inc.keyless SET b = 'after the multixact' WHERE ctid = (SELECT pg_temp.on_page(2, false));
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'after multixact')::text) AS committed;

-- A keyless row with a value out of line can't be held in an entry (its
-- value may be gone when the entry is read): the table is read in full,
-- and only the full read counts.
SET debug_toast_relation = on;
INSERT INTO inc.keyless
  SELECT 5000, string_agg(md5(g::text), '') FROM generate_series(1, 400) g;
RESET debug_toast_relation;
SELECT length(dolt_commit('-am', 'out of line')::text) AS committed;
DELETE FROM inc.keyless WHERE a = 5000;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'out of line gone')::text) AS committed;

-- A value stored by content stays (nothing deletes it with the row, and a
-- collection keeps what the log's pages name): its entry holds the
-- pointer, and the fold reads from the baseline.
INSERT INTO inc.keyless
  SELECT 5001, string_agg(md5(g::text), '') FROM generate_series(1, 400) g;
SELECT length(dolt_commit('-am', 'by content')::text) AS committed;
DELETE FROM inc.keyless WHERE a = 5001;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'by content gone')::text) AS committed;

-- A table redefined, or rewritten, is read in full once.
ALTER TABLE inc.big ADD COLUMN w int;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'a column')::text) AS committed;
SELECT * FROM pg_temp.fold();
TRUNCATE inc.keyless;
INSERT INTO inc.keyless VALUES (1, 'one');
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'rewritten')::text) AS committed;

-- TRUNCATE where the table's storage was made in the transaction, after a
-- commit in it folded the table: the storage is made anew, as a TRUNCATE
-- elsewhere makes it, not cut in place, which the next fold wouldn't see
-- (it reads only the pages listed).  The commit keeps the rows, for a
-- branch made from it and a hard reset.
BEGIN;
CREATE TABLE inc.made (id int PRIMARY KEY, v text);
INSERT INTO inc.made SELECT g, 'x' || g FROM generate_series(1, 5000) g;
SELECT length(dolt_commit('-Am', 'made')::text) AS committed;
SELECT dolt_branch('made');
SELECT pg_relation_filenode('inc.made') AS made \gset
TRUNCATE inc.made;
SELECT pg_relation_filenode('inc.made') <> :made AS made_anew;
COMMIT;
SELECT * FROM pg_temp.fold();
SELECT table_name, status FROM dolt_status;
SELECT dolt_checkout('made');
SELECT count(*) FROM inc.made;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard');
SELECT count(*) FROM inc.made;
-- storage made anew in the transaction, and rows inserted after the
-- TRUNCATE: the working set has just those
BEGIN;
TRUNCATE inc.made;
INSERT INTO inc.made SELECT g, 'y' || g FROM generate_series(1, 5000) g;
SELECT length(dolt_commit('-am', 'made again')::text) AS committed;
SELECT dolt_branch('made_again');
TRUNCATE inc.made;
INSERT INTO inc.made SELECT g, 'z' || g FROM generate_series(1, 10) g;
COMMIT;
SELECT * FROM pg_temp.fold();
SELECT count(*), min(v), max(v) FROM inc.made;
SELECT diff_type, count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::inc.made)
  GROUP BY 1 ORDER BY 1;
SELECT dolt_checkout('made_again');
SELECT count(*), count(*) FILTER (WHERE v LIKE 'y%') AS made_again FROM inc.made;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard');
SELECT count(*), count(*) FILTER (WHERE v LIKE 'y%') AS made_again FROM inc.made;
-- storage made in the transaction and not folded is still cut in place
BEGIN;
CREATE TABLE inc.unfolded (id int);
INSERT INTO inc.unfolded SELECT generate_series(1, 1000);
SELECT pg_relation_filenode('inc.unfolded') AS unfolded \gset
TRUNCATE inc.unfolded;
SELECT pg_relation_filenode('inc.unfolded') = :unfolded AS in_place;
COMMIT;
SELECT dolt_branch('-D', 'made');
SELECT dolt_branch('-D', 'made_again');
DROP TABLE inc.made, inc.unfolded;
SELECT length(dolt_commit('-Am', 'made gone')::text) AS committed;

-- A primary key updated: the old key and the new are both candidates.
UPDATE inc.big SET id = 30000 WHERE id = 11;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'a key changed')::text) AS committed;

-- A key value stored out of line: the entry holds the key itself.
CREATE TABLE inc.wide (k text PRIMARY KEY, v int);
ALTER TABLE inc.wide ALTER k SET STORAGE EXTERNAL;
INSERT INTO inc.wide VALUES (repeat('k', 2500), 1), ('short', 1);
SELECT length(dolt_commit('-Am', 'wide')::text) AS committed;
UPDATE inc.wide SET v = 2 WHERE k = repeat('k', 2500);
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'wide changed')::text) AS committed;

-- Keyless rows of nulls, and copies of a row taken to none and back to
-- one: counted as copies like any other row.
INSERT INTO inc.keyless VALUES (NULL, NULL), (NULL, NULL), (NULL, 'x'),
	(77, 'dup'), (77, 'dup');
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'nulls and copies')::text) AS committed;
BEGIN;
DELETE FROM inc.keyless WHERE a = 77 OR (a IS NULL AND b IS NULL);
INSERT INTO inc.keyless VALUES (77, 'dup'), (NULL, NULL);
COMMIT;
SELECT * FROM pg_temp.fold();
SELECT count(*) FILTER (WHERE a = 77) AS dups,
	count(*) FILTER (WHERE a IS NULL AND b IS NULL) AS nulls FROM inc.keyless;
SELECT length(dolt_commit('-am', 'none and back')::text) AS committed;

-- A checkpoint folds as a commit does: its fold is the baseline, and
-- consumes the entries it read.
UPDATE inc.big SET v = 'checkpointed' WHERE id = 12;
SELECT prolly_checkpoint() IS NOT NULL AS checkpointed;
SELECT * FROM pg_temp.fold();
SELECT table_name, status FROM dolt_status;
SELECT length(dolt_commit('-am', 'checkpointed')::text) AS committed;

-- Folds use the change finder the setting names: "pages" reads pages,
-- and a baseline the other finder made is read in full once, either way.
-- (every versioned table of the database is read in full once, other
-- tests' too: how many isn't this test's)
SET debug_prolly_change_finder = pages;
UPDATE inc.big SET v = 'by pages' WHERE id = 13;
SELECT pages, candidates, full_tables > 0 AS read_in_full, same_as_full
FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'by pages')::text) AS committed;
UPDATE inc.big SET v = 'by pages again' WHERE id = 13;
SELECT pages > 0 AS pages_read, candidates, full_tables, same_as_full
FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'by pages again')::text) AS committed;
SET debug_prolly_change_finder = log;
UPDATE inc.big SET v = 'by log' WHERE id = 13;
SELECT pages, candidates, full_tables > 0 AS read_in_full, same_as_full
FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'by log')::text) AS committed;
UPDATE inc.big SET v = 'by log again' WHERE id = 13;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'by log again')::text) AS committed;
DROP TABLE inc.wide;
SELECT length(dolt_commit('-Am', 'wide gone')::text) AS committed;

-- A branch starts from its source's baseline.
SELECT dolt_branch('other');
SELECT dolt_checkout('other');
UPDATE inc.big SET v = 'other' WHERE id = 3;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'on other')::text) AS committed;
SELECT * FROM pg_temp.fold();
SELECT dolt_checkout('main');
SELECT * FROM pg_temp.fold();

DROP SCHEMA inc CASCADE;
SELECT dolt_branch('-D', 'other');
