-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": 1}');
-- ours
UPDATE t SET j = '{"a": 1, "b": 2}';
-- theirs
UPDATE t SET j = '{"a": 1, "b": 2, "c": 3}';
-- expect
clean
check SELECT j FROM rt.t => {"a": 1, "b": 2, "c": 3}
