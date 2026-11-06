-- base
CREATE TABLE t (pk int PRIMARY KEY, c integer);
INSERT INTO t VALUES (1, 1);
-- ours
ALTER TABLE t ALTER COLUMN c TYPE bigint;
UPDATE t SET c = 99 WHERE pk = 1;
-- theirs
INSERT INTO t VALUES (2, 5);
-- expect
conflict column t.c
