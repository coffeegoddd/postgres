--
-- Semantics: constraints and triggers.  Unique (including deferred),
-- foreign keys, check and exclusion constraints, and row, statement and
-- transition-table triggers behave exactly as on heap.
--
CREATE TABLE parent (id int PRIMARY KEY, name text UNIQUE);
CREATE TABLE child (
	id int PRIMARY KEY,
	parent_id int REFERENCES parent ON DELETE CASCADE ON UPDATE CASCADE,
	qty int CHECK (qty >= 0)
);
INSERT INTO parent VALUES (1, 'one'), (2, 'two'), (3, 'three');
INSERT INTO child VALUES (10, 1, 5), (11, 1, 6), (20, 2, 7);

-- Foreign keys.
INSERT INTO child VALUES (30, 99, 1);
UPDATE parent SET id = 100 WHERE id = 1;
DELETE FROM parent WHERE id = 2;
SELECT * FROM child ORDER BY id;
CREATE TABLE strict_child (id int, parent_id int REFERENCES parent ON DELETE RESTRICT);
INSERT INTO strict_child VALUES (1, 3);
DELETE FROM parent WHERE id = 3;

-- Check and NOT NULL.
INSERT INTO child VALUES (40, 100, -1);
UPDATE child SET qty = -5 WHERE id = 10;

-- Deferred unique constraint: checked at commit.
CREATE TABLE deferred (a int UNIQUE DEFERRABLE INITIALLY DEFERRED);
BEGIN;
INSERT INTO deferred VALUES (1), (1);
DELETE FROM deferred WHERE ctid = (SELECT min(ctid) FROM deferred);
COMMIT;
SELECT * FROM deferred;
BEGIN;
INSERT INTO deferred VALUES (1);
COMMIT;
UPDATE deferred SET a = a;

-- Exclusion constraint.
CREATE EXTENSION IF NOT EXISTS btree_gist;
CREATE TABLE booking (room int, during int4range,
					  EXCLUDE USING gist (room WITH =, during WITH &&));
INSERT INTO booking VALUES (1, '[1,5)'), (1, '[5,9)'), (2, '[1,5)');
INSERT INTO booking VALUES (1, '[4,6)');

-- Triggers.
CREATE TABLE audit (op text, old_id int, new_id int, note text);
CREATE FUNCTION child_before() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	NEW.qty := NEW.qty * 10;
	RETURN NEW;
END $$;
CREATE FUNCTION child_after() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO audit VALUES (TG_OP, OLD.id, NEW.id, NULL);
	RETURN NULL;
END $$;
CREATE FUNCTION child_stmt() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO audit SELECT TG_OP, NULL, NULL, 'rows: ' || count(*) FROM new_rows;
	RETURN NULL;
END $$;
CREATE TRIGGER child_before BEFORE INSERT ON child
	FOR EACH ROW EXECUTE FUNCTION child_before();
CREATE TRIGGER child_after AFTER INSERT OR UPDATE OR DELETE ON child
	FOR EACH ROW EXECUTE FUNCTION child_after();
CREATE TRIGGER child_stmt AFTER UPDATE ON child
	REFERENCING NEW TABLE AS new_rows FOR EACH STATEMENT EXECUTE FUNCTION child_stmt();
INSERT INTO child VALUES (50, 100, 3);
UPDATE child SET qty = qty + 1 WHERE parent_id = 100;
DELETE FROM child WHERE id = 50;
SELECT * FROM audit ORDER BY op, old_id NULLS FIRST, new_id NULLS FIRST, note;
SELECT * FROM child ORDER BY id;

DROP TABLE booking, deferred, strict_child, child, parent, audit;
DROP FUNCTION child_before(), child_after(), child_stmt();

-- Keys updated to an equal value written differently (numeric scale): the
-- update replaces the row, and the new spelling is what reads return.
CREATE TABLE eqk (k numeric PRIMARY KEY, n numeric);
CREATE INDEX eqk_n ON eqk (n);
INSERT INTO eqk VALUES (1.0, 2.0), (3, 4);
UPDATE eqk SET k = 1.00 WHERE k = 1;
UPDATE eqk SET n = 2.000 WHERE k = 1;
SELECT k::text, n::text FROM eqk ORDER BY k;
SET enable_seqscan = off;
SELECT k::text FROM eqk WHERE k = 1;
SELECT n::text FROM eqk WHERE n = 2;
RESET enable_seqscan;
BEGIN;
UPDATE eqk SET k = 3.0 WHERE k = 3;
SAVEPOINT s;
UPDATE eqk SET k = 3.00 WHERE k = 3;
ROLLBACK TO s;
SELECT k::text FROM eqk ORDER BY k;
COMMIT;
SELECT k::text FROM eqk ORDER BY k;
DROP TABLE eqk;
