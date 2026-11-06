-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);

-- ours
INSERT INTO t VALUES (2, 1), (3, 3);
-- theirs
INSERT INTO t VALUES (1, 1), (4, 4);
-- expect
violation unique t t_col1_key 1
violation unique t t_col1_key 2
