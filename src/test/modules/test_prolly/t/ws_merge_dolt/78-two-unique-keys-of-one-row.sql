-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CONSTRAINT u1 UNIQUE (col1), CONSTRAINT u2 UNIQUE (col2));
-- ours
INSERT INTO t VALUES (1, 1, 1);
-- theirs
INSERT INTO t VALUES (2, 1, 1);
-- expect
violation unique t u1 1
violation unique t u1 2
violation unique t u2 1
violation unique t u2 2
