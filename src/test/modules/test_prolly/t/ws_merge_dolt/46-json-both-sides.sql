-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{}');
-- ours
UPDATE t SET j = '{"a": 1}';
-- theirs
UPDATE t SET j = '{"b": 2}';
-- expect
clean
check SELECT j FROM rt.t => {"a": 1, "b": 2}
