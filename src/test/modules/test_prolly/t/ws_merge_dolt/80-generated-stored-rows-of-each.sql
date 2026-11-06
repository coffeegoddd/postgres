-- base
CREATE TABLE t1 (id bigint PRIMARY KEY, v1 bigint, v2 bigint, v3 bigint GENERATED ALWAYS AS (v1 + v2) STORED);
CREATE INDEX t1_v3 ON t1 (v3);
INSERT INTO t1 (id, v1, v2) VALUES (1, 1, 1), (2, 2, 2);
-- ours
INSERT INTO t1 (id, v1, v2) VALUES (3, 3, 3);
-- theirs
INSERT INTO t1 (id, v1, v2) VALUES (4, 4, 4);
-- expect
clean
check SELECT string_agg(v3::text, ',' ORDER BY id) FROM rt.t1 => 2,4,6,8
