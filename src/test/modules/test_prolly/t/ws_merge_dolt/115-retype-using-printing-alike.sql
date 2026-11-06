-- base
CREATE TABLE t (pk int PRIMARY KEY, n integer);
INSERT INTO t VALUES (1, 1);
-- ours
ALTER TABLE t ALTER COLUMN n TYPE bigint USING n * 100;
-- theirs
INSERT INTO t VALUES (2, 2);
-- expect
conflict column t.n
