-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 1);
-- ours
ALTER TABLE t ADD COLUMN col2 int NOT NULL DEFAULT 0;
ALTER TABLE t ADD COLUMN col3 int;
INSERT INTO t VALUES (2, 2, 2, NULL);
-- theirs
INSERT INTO t VALUES (3, 3);
-- expect
clean
check SELECT string_agg(pk || ':' || col2, ',' ORDER BY pk) FROM rt.t => 1:0,2:2,3:0
