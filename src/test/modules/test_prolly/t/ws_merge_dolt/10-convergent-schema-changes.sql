-- base
CREATE TABLE parent (id int PRIMARY KEY);
INSERT INTO parent VALUES (1), (2), (3);
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 10);
-- ours
ALTER TABLE t ALTER COLUMN col1 SET NOT NULL;
ALTER TABLE t ADD COLUMN col3 int;
CREATE INDEX idx1 ON t (col3, col1);
ALTER TABLE t ADD CONSTRAINT fk1 FOREIGN KEY (col3) REFERENCES parent (id);
-- theirs
ALTER TABLE t ALTER COLUMN col1 SET NOT NULL;
ALTER TABLE t ADD COLUMN col3 int;
CREATE INDEX idx1 ON t (col3, col1);
UPDATE t SET col1 = -1000 WHERE pk = 1;
ALTER TABLE t ADD CONSTRAINT fk1 FOREIGN KEY (col3) REFERENCES parent (id);
-- expect
clean
check SELECT col1, col3 IS NULL FROM rt.t => -1000|t
check SELECT attnotnull FROM pg_attribute WHERE attrelid = 'rt.t'::regclass AND attname = 'col1' => t
check SELECT count(*) FROM pg_constraint WHERE conname = 'fk1' => 1
