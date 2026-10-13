--
-- Semantics: transactions within one session.  Savepoints, subtransactions,
-- command visibility (Halloween protection), ON CONFLICT, cursors, ctid
-- lookups, xmin as a change token, and two-phase commit behave exactly as
-- on heap.
--
CREATE TABLE tx (id int PRIMARY KEY, v int);

-- Savepoints.
BEGIN;
INSERT INTO tx VALUES (1, 1);
SAVEPOINT a;
INSERT INTO tx VALUES (2, 2);
SAVEPOINT b;
INSERT INTO tx VALUES (3, 3);
ROLLBACK TO b;
UPDATE tx SET v = 20 WHERE id = 2;
SAVEPOINT c;
DELETE FROM tx WHERE id = 1;
ROLLBACK TO c;
RELEASE a;
COMMIT;
SELECT * FROM tx ORDER BY id;

-- An error inside a PL/pgSQL exception block rolls back only that block.
DO $$
BEGIN
	INSERT INTO tx VALUES (10, 10);
	BEGIN
		INSERT INTO tx VALUES (11, 11);
		INSERT INTO tx VALUES (10, 0);	-- duplicate key
	EXCEPTION WHEN unique_violation THEN
		RAISE NOTICE 'caught duplicate';
	END;
END $$;
SELECT * FROM tx ORDER BY id;

-- Halloween protection: a statement doesn't see its own changes.
CREATE TABLE hw (a int PRIMARY KEY);
INSERT INTO hw SELECT generate_series(1, 1000);
UPDATE hw SET a = a + 1000;
SELECT count(*), min(a), max(a) FROM hw;
INSERT INTO hw SELECT a + 5000 FROM hw;
SELECT count(*), min(a), max(a) FROM hw;

-- Later commands in a transaction see earlier ones.
BEGIN;
INSERT INTO tx VALUES (20, 20);
UPDATE tx SET v = v + 1 WHERE id = 20;
UPDATE tx SET v = v + 1 WHERE id = 20;
SELECT v FROM tx WHERE id = 20;
DELETE FROM tx WHERE id = 20;
SELECT count(*) FROM tx WHERE id = 20;
COMMIT;

-- Data-modifying CTEs.
WITH moved AS (DELETE FROM tx WHERE id = 2 RETURNING *)
INSERT INTO tx SELECT id + 100, v FROM moved;
SELECT * FROM tx ORDER BY id;

-- ON CONFLICT.
INSERT INTO tx VALUES (1, 100) ON CONFLICT (id) DO NOTHING;
INSERT INTO tx VALUES (1, 100), (30, 30) ON CONFLICT (id) DO UPDATE SET v = excluded.v + tx.v;
INSERT INTO tx VALUES (1, 0) ON CONFLICT (id) DO UPDATE SET v = -1 WHERE tx.v > 1000;
SELECT * FROM tx ORDER BY id;

-- Cursors: WHERE CURRENT OF, scrolling, holdable.
BEGIN;
DECLARE c CURSOR FOR SELECT * FROM tx ORDER BY id FOR UPDATE;
FETCH 2 FROM c;
UPDATE tx SET v = 777 WHERE CURRENT OF c;
CLOSE c;
DECLARE s SCROLL CURSOR FOR SELECT id FROM tx ORDER BY id;
FETCH LAST FROM s;
FETCH ABSOLUTE 1 FROM s;
COMMIT;
SELECT * FROM tx ORDER BY id;

-- ctid identifies a row version; TID scans find it.
SELECT ctid AS tid FROM tx WHERE id = 30 \gset
SELECT id FROM tx WHERE ctid = :'tid';
UPDATE tx SET v = v + 1 WHERE id = 30;
SELECT count(*) AS old_tid_finds FROM tx WHERE ctid = :'tid';
SELECT ctid AS tid2 FROM tx WHERE id = 30 \gset
SELECT id AS new_tid_finds FROM tx WHERE ctid = :'tid2';

-- xmin changes when a row changes, and only then.
SELECT xmin AS x1 FROM tx WHERE id = 1 \gset
SELECT xmin AS x102 FROM tx WHERE id = 102 \gset
UPDATE tx SET v = v + 1 WHERE id = 1;
SELECT (SELECT xmin::text FROM tx WHERE id = 1) <> :'x1' AS changed_row_new_xmin,
	   (SELECT xmin::text FROM tx WHERE id = 102) = :'x102' AS other_row_same_xmin;

-- Two-phase commit.
BEGIN;
INSERT INTO tx VALUES (40, 40);
PREPARE TRANSACTION 'p1';
BEGIN;
INSERT INTO tx VALUES (41, 41);
PREPARE TRANSACTION 'p2';
SELECT gid FROM pg_prepared_xacts ORDER BY gid;
SELECT count(*) FROM tx WHERE id IN (40, 41);
COMMIT PREPARED 'p1';
ROLLBACK PREPARED 'p2';
SELECT id FROM tx WHERE id IN (40, 41);

-- Explicit row locks in one session.
BEGIN;
SELECT id FROM tx WHERE id = 1 FOR UPDATE;
SELECT id FROM tx WHERE id = 30 FOR NO KEY UPDATE;
SELECT id FROM tx WHERE id = 10 FOR SHARE;
SELECT id FROM tx WHERE id = 102 FOR KEY SHARE;
COMMIT;

DROP TABLE tx, hw;
