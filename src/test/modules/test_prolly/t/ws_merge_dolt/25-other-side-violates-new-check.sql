-- base
CREATE TABLE t (pk int PRIMARY KEY, col00 int, col01 int, col1 varchar(100) DEFAULT 'hello');
INSERT INTO t VALUES (1, 0, 0, 'hi');
-- ours
INSERT INTO t VALUES (2, 0, 0, DEFAULT);
-- theirs
ALTER TABLE t DROP COLUMN col00; ALTER TABLE t DROP COLUMN col01;
ALTER TABLE t ADD CONSTRAINT c1 CHECK (col1 != 'hello');
-- expect
violation check t c1 2
