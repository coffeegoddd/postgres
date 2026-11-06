-- base
CREATE TABLE t (pk varchar(100) PRIMARY KEY, col1 int, col2 varchar(100), CONSTRAINT unique1 UNIQUE (col2));
INSERT INTO t VALUES ('0', 0, '');
-- ours
ALTER TABLE t DROP COLUMN col1;
INSERT INTO t (pk, col2) VALUES ('10', 'same');
-- theirs
INSERT INTO t VALUES ('1', 10, 'same');
-- expect
violation unique t unique1 1
violation unique t unique1 10
check SELECT string_agg(pk || ':' || col2, ',' ORDER BY pk) FROM rt.t => 0:,1:same,10:same
