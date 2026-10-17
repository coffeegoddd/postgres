-- A commit folds only what changed since the branch's last commit: the
-- rows on the pages its tables list as changed.  The tree is always the
-- one a fold of every row gives, and what it read is counted.

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
-- copies (each row here has ten): they are counted again from the pages
-- changed, as they were and as they are, not read in full.
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
-- truncated
DELETE FROM inc.keyless WHERE a = 10;
DELETE FROM inc.keyless WHERE ctid >= '(4,0)';
VACUUM inc.keyless;
SELECT pg_relation_size('inc.keyless') / 8192 AS keyless_pages;
SELECT * FROM pg_temp.fold();
-- in the transaction writing: its own writes are counted; its commit's
-- fold saw rows not yet settled, so the next fold reads in full once
BEGIN;
UPDATE inc.keyless SET b = 'mine' WHERE a = 11;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless again')::text) AS committed;
COMMIT;
UPDATE inc.keyless SET b = 'after' WHERE a = 12;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless after')::text) AS committed;
UPDATE inc.keyless SET b = 'later' WHERE a = 13;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'keyless later')::text) AS committed;

-- A table redefined, or rewritten, is read in full once.
ALTER TABLE inc.big ADD COLUMN w int;
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'a column')::text) AS committed;
SELECT * FROM pg_temp.fold();
TRUNCATE inc.keyless;
INSERT INTO inc.keyless VALUES (1, 'one');
SELECT * FROM pg_temp.fold();
SELECT length(dolt_commit('-am', 'rewritten')::text) AS committed;

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
