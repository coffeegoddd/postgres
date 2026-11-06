-- base
CREATE TABLE t (pk int PRIMARY KEY, a int, other int);
INSERT INTO t VALUES (1, 1, 1);
-- ours
ALTER TABLE t DROP COLUMN a;
ALTER TABLE t RENAME COLUMN other TO a;
-- theirs
UPDATE t SET other = 101;
-- expect
clean
check SELECT a FROM rt.t => 101
