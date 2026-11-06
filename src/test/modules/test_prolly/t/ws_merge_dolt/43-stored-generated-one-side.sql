-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1);
-- ours
ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 1) STORED;
INSERT INTO t (pk) VALUES (3);
-- theirs
INSERT INTO t (pk) VALUES (2);
-- expect
clean
check SELECT string_agg(pk || ':' || col1, ',' ORDER BY pk) FROM rt.t => 1:2,2:3,3:4
