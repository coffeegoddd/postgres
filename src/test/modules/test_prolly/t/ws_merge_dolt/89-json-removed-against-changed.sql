-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": 1, "b": 2}');
-- ours
UPDATE t SET j = '{"b": 2}';
-- theirs
UPDATE t SET j = '{"a": 9, "b": 2}';
-- expect
conflict row t 1
