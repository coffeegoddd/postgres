-- base
CREATE TABLE t (pk int PRIMARY KEY, n integer, o integer);
INSERT INTO t VALUES (1, 1, 1);
-- ours
ALTER TABLE t ALTER COLUMN n TYPE numeric(10,2) USING n / 100.0;
-- theirs
ALTER TABLE t ADD COLUMN z integer;
-- expect
clean
check SELECT n FROM rt.t => 0.01
