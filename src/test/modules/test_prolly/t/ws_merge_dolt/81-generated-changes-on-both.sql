-- base
CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint, v3 bigint GENERATED ALWAYS AS (v1 + v2) STORED);
INSERT INTO t1 (id, v1, v2) VALUES (1, 1, 1), (2, 2, 2);
-- ours
UPDATE t1 SET v1 = 4 WHERE id = 1;
-- theirs
UPDATE t1 SET v2 = 5 WHERE id = 1;
-- expect
clean
check SELECT string_agg(v3::text, ',' ORDER BY id) FROM rt.t1 => 9,4
