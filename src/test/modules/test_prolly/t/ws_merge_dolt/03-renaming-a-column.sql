-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));
INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');
CREATE INDEX idx1 ON t (col1, pk);
-- ours
ALTER TABLE t RENAME COLUMN col1 TO col11;
INSERT INTO t VALUES (3, 30, '300'), (4, 40, '400');
-- theirs
INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');
-- expect
clean
check SELECT string_agg(pk || ':' || col11, ',' ORDER BY pk) FROM rt.t => 1:10,2:20,3:30,4:40,5:50,6:60
