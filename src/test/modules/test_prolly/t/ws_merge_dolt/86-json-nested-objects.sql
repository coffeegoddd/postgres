-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": {"x": 1, "y": 1}, "k": 1}');
-- ours
UPDATE t SET j = '{"a": {"x": 2, "y": 1}, "k": 1}';
-- theirs
UPDATE t SET j = '{"a": {"x": 1, "y": 3}, "k": 1}';
-- expect
clean
check SELECT j FROM rt.t => {"a": {"x": 2, "y": 3}, "k": 1}
