-- base
CREATE TABLE t (col1 int NOT NULL, col2 int NOT NULL, col3 int, PRIMARY KEY (col1, col2), CONSTRAINT u UNIQUE (col2, col3));
-- ours
INSERT INTO t VALUES (1, 2, NULL);
-- theirs
INSERT INTO t VALUES (2, 2, NULL);
-- expect
clean
check SELECT count(*) FROM rt.t => 2
