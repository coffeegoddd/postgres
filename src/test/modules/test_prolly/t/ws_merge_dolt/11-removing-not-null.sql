-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int NOT NULL);
INSERT INTO t VALUES (1, 1), (2, 2);
-- ours
ALTER TABLE t ALTER COLUMN col1 DROP NOT NULL;
INSERT INTO t VALUES (3, NULL);
-- theirs
INSERT INTO t VALUES (4, 4);
-- expect
clean
check SELECT string_agg(pk || ':' || coalesce(col1::text, 'null'), ',' ORDER BY pk) FROM rt.t => 1:1,2:2,3:null,4:4
