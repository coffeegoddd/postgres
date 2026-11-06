-- base
CREATE TABLE t (pk int, col1 int);
INSERT INTO t VALUES (1, 1);
-- ours
ALTER TABLE t ADD CONSTRAINT t_col1_key UNIQUE (col1);
-- theirs
INSERT INTO t VALUES (2, 1), (3, 1);
-- expect
violation unique t t_col1_key (1,1)
violation unique t t_col1_key (2,1)
violation unique t t_col1_key (3,1)
