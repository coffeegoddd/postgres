-- base
CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));
INSERT INTO t VALUES (1, NULL);
-- ours
ALTER TABLE t ALTER COLUMN c1 SET DEFAULT 'hello';
INSERT INTO t VALUES (2, DEFAULT);
-- theirs
INSERT INTO t VALUES (3, NULL);
-- expect
clean
check SELECT string_agg(coalesce(c1, 'null'), ',' ORDER BY pk) FROM rt.t => null,hello,null
