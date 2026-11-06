-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int UNIQUE);
INSERT INTO t VALUES (1, 1);
-- ours
ALTER TABLE t DROP COLUMN col1; ALTER TABLE t ADD COLUMN col1 int;
UPDATE t SET col1 = 1 WHERE pk = 1;
INSERT INTO t VALUES (2, 1);
-- theirs
INSERT INTO t VALUES (3, 3);
-- expect
clean
check SELECT string_agg(pk || ':' || coalesce(col1::text, 'null'), ',' ORDER BY pk) FROM rt.t => 1:1,2:1,3:3
