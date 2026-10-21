-- The row applier writes rows as logical replication's apply does: with
-- index maintenance and constraint checks, without user triggers or
-- foreign key checks.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.p (id int PRIMARY KEY);
CREATE TABLE app.t (id int PRIMARY KEY, v text NOT NULL CHECK (v <> 'bad'),
                    u int UNIQUE, p int REFERENCES app.p);
CREATE INDEX t_v ON app.t (v);
CREATE TABLE app.k (a text, b int);		-- keyless
CREATE TABLE app.log (what text);
CREATE FUNCTION app.no_writes() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN INSERT INTO app.log VALUES (TG_OP); RETURN NEW; END$$;
CREATE TRIGGER t_trig BEFORE INSERT OR UPDATE OR DELETE ON app.t
  FOR EACH ROW EXECUTE FUNCTION app.no_writes();
CREATE TRIGGER t_always AFTER INSERT OR UPDATE OR DELETE ON app.t
  FOR EACH ROW EXECUTE FUNCTION app.no_writes();
ALTER TABLE app.t ENABLE ALWAYS TRIGGER t_always;

-- Rows by key.
SELECT test_apply('insert', NULL, ROW(1, 'one', 10, NULL)::app.t);
SELECT test_apply('insert', NULL, ROW(2, 'two', 20, NULL)::app.t);
SELECT test_apply('update', ROW(1, 'ignored', NULL, NULL)::app.t, ROW(1, 'uno', 11, NULL)::app.t);
SELECT test_apply('remove', ROW(2, NULL, NULL, NULL)::app.t, NULL::app.t);
SELECT test_apply('update', ROW(9, NULL, NULL, NULL)::app.t, ROW(9, 'nine', 90, NULL)::app.t) AS found;
SELECT test_apply('remove', ROW(9, NULL, NULL, NULL)::app.t, NULL::app.t) AS found;
SELECT * FROM app.t ORDER BY id;
-- indexes are kept
SET enable_seqscan = off;
SELECT id FROM app.t WHERE v = 'uno';
RESET enable_seqscan;
-- no user triggers fired (ALWAYS ones neither), no foreign key checked
SELECT count(*) AS triggered FROM app.log;
SELECT test_apply('insert', NULL, ROW(3, 'three', 30, 999)::app.t);
SELECT * FROM app.t WHERE id = 3;
-- but NOT NULL, CHECK and unique constraints hold
SELECT test_apply('insert', NULL, ROW(4, NULL, 40, NULL)::app.t);
SELECT test_apply('insert', NULL, ROW(4, 'bad', 40, NULL)::app.t);
SELECT test_apply('insert', NULL, ROW(4, 'four', 11, NULL)::app.t);
SELECT test_apply('insert', NULL, ROW(1, 'dup', 41, NULL)::app.t);
-- and triggers fire again afterwards
INSERT INTO app.t VALUES (5, 'five', 50, NULL);
SELECT count(*) AS triggered FROM app.log;

-- A keyless table's rows: one copy at a time, matched by every column.
SELECT test_apply('insert', NULL, ROW('x', 1)::app.k);
SELECT test_apply('insert', NULL, ROW('x', 1)::app.k);
SELECT test_apply('insert', NULL, ROW('y', NULL)::app.k);
SELECT test_apply('remove', ROW('x', 1)::app.k, NULL::app.k);
SELECT test_apply('update', ROW('y', NULL)::app.k, ROW('z', 2)::app.k);
SELECT test_apply('remove', ROW('w', 1)::app.k, NULL::app.k) AS found;
SELECT * FROM app.k ORDER BY a;

-- Copies match by their bytes, not by their types' equality, through an
-- index on the table or not, and types without equality work.
CREATE TABLE app.kb (a text, n numeric, f float8, pt point);
CREATE INDEX kb_n ON app.kb (n);
SELECT test_apply('insert', NULL, ROW('x', 1.0, 0, '(1,1)')::app.kb);
SELECT test_apply('insert', NULL, ROW('x', 1.00, '-0', '(1,1)')::app.kb);
SELECT test_apply('remove', ROW('x', 1.00, 0, '(1,1)')::app.kb, NULL::app.kb) AS found;
SELECT test_apply('remove', ROW('x', 1.00, '-0', '(1,1)')::app.kb, NULL::app.kb);
SELECT test_apply('update', ROW('x', 1.0, 0, '(1,1)')::app.kb, ROW('y', NULL, 1, '(2,2)')::app.kb);
SELECT test_apply('remove', ROW('y', NULL, 1, '(2,2)')::app.kb, NULL::app.kb);
SELECT count(*) AS left FROM app.kb;
-- an index that can't search for NULL isn't used for one
CREATE TABLE app.kh (a int, b text);
CREATE INDEX kh_a ON app.kh USING hash (a);
SELECT test_apply('insert', NULL, ROW(NULL, 'n')::app.kh);
SELECT test_apply('remove', ROW(NULL, 'n')::app.kh, NULL::app.kh);
SELECT count(*) AS left FROM app.kh;

-- Deferrable constraints: the primary key finds rows, unique ones are
-- checked when due.
CREATE TABLE app.d (id int PRIMARY KEY DEFERRABLE, v text,
                    u int UNIQUE DEFERRABLE,
                    w int UNIQUE DEFERRABLE INITIALLY DEFERRED);
SELECT test_apply('insert', NULL, ROW(1, 'one', 1, 1)::app.d);
SELECT test_apply('update', ROW(1, NULL, NULL, NULL)::app.d, ROW(1, 'uno', 1, 1)::app.d);
SELECT * FROM app.d;
SELECT test_apply('insert', NULL, ROW(2, 'two', 1, 2)::app.d);
BEGIN;
SELECT test_apply('insert', NULL, ROW(3, 'three', 3, 1)::app.d);
COMMIT;
SELECT count(*) AS rows FROM app.d;

-- Only tables with storage: a partitioned table's rows go to its
-- partitions.
CREATE TABLE app.pt (id int PRIMARY KEY) PARTITION BY RANGE (id);
CREATE TABLE app.pt1 PARTITION OF app.pt FOR VALUES FROM (0) TO (10);
SELECT test_apply('insert', NULL, ROW(1)::app.pt);
SELECT test_apply('insert', NULL, ROW(1)::app.pt1);
SELECT test_apply('insert', NULL, ROW(11)::app.pt1);
SELECT * FROM app.pt;

-- Writing takes the privileges to select, insert, update and delete,
-- and no row-level security on the table.
CREATE ROLE applier_reader;
GRANT USAGE ON SCHEMA app TO applier_reader;
GRANT SELECT, INSERT ON app.k TO applier_reader;
GRANT INSERT, UPDATE, DELETE ON app.kb TO applier_reader;
CREATE TABLE app.r (id int PRIMARY KEY, tenant text);
GRANT ALL ON app.r TO applier_reader;
ALTER TABLE app.r ENABLE ROW LEVEL SECURITY;
CREATE POLICY r_mine ON app.r USING (tenant = current_user);
SET ROLE applier_reader;
SELECT test_apply('insert', NULL, ROW('q', 1)::app.k);
SELECT test_apply('insert', NULL, ROW('q', 1, 1, '(0,0)')::app.kb);
SELECT test_apply('remove', ROW(1, 'other')::app.r, NULL::app.r);
RESET ROLE;

-- Skipping the checks of foreign keys referencing a table takes UPDATE and
-- DELETE on the tables that hold them, without row-level security there.
CREATE TABLE app.fp (id int PRIMARY KEY);
CREATE TABLE app.fc (id int PRIMARY KEY, p int REFERENCES app.fp ON DELETE RESTRICT);
INSERT INTO app.fp VALUES (1);
INSERT INTO app.fc VALUES (1, 1);
GRANT ALL ON app.fp TO applier_reader;
SET ROLE applier_reader;
SELECT test_apply('remove', ROW(1)::app.fp, NULL::app.fp);
RESET ROLE;
GRANT UPDATE, DELETE ON app.fc TO applier_reader;
ALTER TABLE app.fc ENABLE ROW LEVEL SECURITY;
SET ROLE applier_reader;
SELECT test_apply('remove', ROW(1)::app.fp, NULL::app.fp);
RESET ROLE;
ALTER TABLE app.fc DISABLE ROW LEVEL SECURITY;
SET ROLE applier_reader;
SELECT test_apply('remove', ROW(1)::app.fp, NULL::app.fp);
RESET ROLE;
SELECT * FROM app.fc;
DROP OWNED BY applier_reader;
DROP ROLE applier_reader;

-- A table new in the transaction, filled from nothing by rows inserted
-- together, is loaded: its indexes built at the next write.  A build
-- refusing a duplicate key ends the applier, though its error is caught:
-- a write after it would have no index to keep.  So does a build in a
-- subtransaction begun since the rows were loaded, which may roll the
-- indexes back from under them.
BEGIN;
CREATE TABLE app.ld (id int PRIMARY KEY, v text);
SELECT test_apply_failed_load(array_agg(ROW(g % 150, 'v')::app.ld), true)
FROM generate_series(1, 200) g;
SELECT count(*) FROM app.ld;
ROLLBACK;
BEGIN;
CREATE TABLE app.ld (id int PRIMARY KEY, v text);
SELECT test_apply_failed_load(array_agg(ROW(g, 'v')::app.ld), false)
FROM generate_series(1, 200) g;
SELECT count(*) FROM app.ld;
ROLLBACK;

DROP SCHEMA app CASCADE;
