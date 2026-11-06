-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": [1], "b": 1}');
-- ours
UPDATE t SET j = '{"a": [1, 2], "b": 1}';
-- theirs
UPDATE t SET j = '{"a": [1, 3], "b": 1}';
-- expect
conflict row t 1
