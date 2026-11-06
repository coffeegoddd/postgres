-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int GENERATED ALWAYS AS (pk + 1) VIRTUAL);
INSERT INTO t (pk) VALUES (1);
-- ours
ALTER TABLE t ADD COLUMN col2 int; ALTER TABLE t ADD COLUMN col3 int;
INSERT INTO t (pk, col2, col3) VALUES (2, 4, 5);
-- theirs
INSERT INTO t (pk) VALUES (3);
-- expect
clean
check SELECT string_agg(col1::text, ',' ORDER BY pk) FROM rt.t => 2,3,4
