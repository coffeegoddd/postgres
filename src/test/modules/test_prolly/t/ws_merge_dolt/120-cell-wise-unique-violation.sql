-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CONSTRAINT col1_col2_u UNIQUE (col1, col2));
INSERT INTO t VALUES (1, 1, 1), (2, NULL, NULL);
-- ours
UPDATE t SET col1 = 1 WHERE pk = 2;
-- theirs
UPDATE t SET col2 = 1 WHERE pk = 2;
-- expect
violation unique t col1_col2_u 1
violation unique t col1_col2_u 2
