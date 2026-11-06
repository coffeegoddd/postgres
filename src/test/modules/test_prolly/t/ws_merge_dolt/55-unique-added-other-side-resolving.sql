-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 1), (2, 1);
-- ours
INSERT INTO t VALUES (3, 3);
-- theirs
UPDATE t SET col1 = 2 WHERE pk = 2;
ALTER TABLE t ADD CONSTRAINT col1_uniq UNIQUE (col1);
-- expect
clean
check SELECT string_agg(pk || ':' || col1, ',' ORDER BY pk) FROM rt.t => 1:1,2:2,3:3
