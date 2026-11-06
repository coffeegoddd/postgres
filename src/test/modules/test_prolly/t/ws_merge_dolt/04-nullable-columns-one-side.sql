-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 1);
-- ours
ALTER TABLE t ADD COLUMN col2 int; ALTER TABLE t ADD COLUMN col3 int;
INSERT INTO t VALUES (2, 2, 2, 2);
-- theirs
INSERT INTO t VALUES (3, 3);
-- expect
clean
check SELECT count(*), count(col2), sum(col3) FROM rt.t => 3|1|2
