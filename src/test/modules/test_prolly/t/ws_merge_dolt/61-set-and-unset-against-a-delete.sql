-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1), (2);
ALTER TABLE t ADD COLUMN col1 int;
ALTER TABLE t ADD COLUMN col2 int;
-- ours
UPDATE t SET col1 = 1 WHERE pk = 1;
UPDATE t SET col1 = NULL WHERE pk = 1;
-- theirs
DELETE FROM t WHERE pk = 1;
-- expect
clean
check SELECT string_agg(pk::text, ',') FROM rt.t => 2
