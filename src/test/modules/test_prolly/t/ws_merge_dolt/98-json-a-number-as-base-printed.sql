-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": 1}');
-- ours
UPDATE t SET j = '{"a": 1.0, "o": 1}';
-- theirs
UPDATE t SET j = '{"a": 1, "t": 1}';
-- expect
clean
check SELECT j FROM rt.t => {"a": 1.0, "o": 1, "t": 1}
