-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT 'hello');
INSERT INTO t VALUES (1, 'hi');
-- ours
ALTER TABLE t ADD CONSTRAINT c1 CHECK (col1 != 'hello');
UPDATE t SET col1 = 'bye' WHERE pk = 1;
-- theirs
UPDATE t SET col1 = 'adios' WHERE pk = 1;
-- expect
conflict row t 1
