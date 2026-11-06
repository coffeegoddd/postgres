-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10), col2 int);
INSERT INTO t VALUES (1, '54321', 10);
-- ours
ALTER TABLE t ALTER COLUMN col1 TYPE text;
UPDATE t SET col2 = 40 WHERE col2 = 10;
INSERT INTO t VALUES (2, '43210000000', 20);
-- theirs
INSERT INTO t VALUES (3, '321', 30);
-- expect
clean
check SELECT string_agg(col1 || ':' || col2, ',' ORDER BY pk) FROM rt.t => 54321:40,43210000000:20,321:30
