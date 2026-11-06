-- base
CREATE TABLE t (pk int PRIMARY KEY, a int, other int);
INSERT INTO t VALUES (1, 1, 1), (2, 2, 2);
-- ours
ALTER TABLE t DROP COLUMN a;
ALTER TABLE t RENAME COLUMN other TO a;
-- theirs
DELETE FROM t WHERE pk = 1;
-- expect
clean
check SELECT string_agg(pk || ':' || a, ',') FROM rt.t => 2:2
