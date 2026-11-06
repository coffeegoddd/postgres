-- base
CREATE TABLE t (a int, b int UNIQUE);
-- ours
INSERT INTO t VALUES (1, 1);
-- theirs
INSERT INTO t VALUES (2, 1);
-- expect
violation unique t t_b_key (1,1)
violation unique t t_b_key (2,1)
