-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100), UNIQUE (col2, pk));
INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');
CREATE INDEX idx1 ON t (pk, col2);
-- ours
ALTER TABLE t DROP COLUMN col1;
INSERT INTO t VALUES (3, '300'), (4, '400');
-- theirs
INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');
-- expect
clean
check SELECT string_agg(pk || ':' || col2, ',' ORDER BY pk) FROM rt.t => 1:100,2:200,3:300,4:400,5:500,6:600
