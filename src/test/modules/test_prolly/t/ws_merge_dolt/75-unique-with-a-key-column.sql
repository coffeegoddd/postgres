-- base
CREATE TABLE t (col1 int NOT NULL, col2 int NOT NULL, col3 int, PRIMARY KEY (col1, col2), CONSTRAINT u UNIQUE (col2, col3));
-- ours
INSERT INTO t VALUES (1, 2, 3);
-- theirs
INSERT INTO t VALUES (2, 2, 3);
-- expect
violation unique t u 1,2
violation unique t u 2,2
