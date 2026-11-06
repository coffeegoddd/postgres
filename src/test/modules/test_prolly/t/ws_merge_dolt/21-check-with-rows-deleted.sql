-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, col3 int, CHECK (col2 != col3));
INSERT INTO t VALUES (1, 2, 3, -3);
-- ours
DELETE FROM t WHERE pk = 1;
-- theirs
INSERT INTO t VALUES (4, 3, 2, 1);
-- expect
clean
