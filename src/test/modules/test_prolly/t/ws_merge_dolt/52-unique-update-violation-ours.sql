-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);
INSERT INTO t VALUES (1, 1), (2, 2);
-- ours
INSERT INTO t VALUES (3, 3);
-- theirs
UPDATE t SET col1 = 3 WHERE pk = 2;
-- expect
violation unique t t_col1_key 2
violation unique t t_col1_key 3
