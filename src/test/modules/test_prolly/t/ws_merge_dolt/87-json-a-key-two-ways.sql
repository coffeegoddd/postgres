-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": 1}');
-- ours
UPDATE t SET j = '{"a": 2}';
-- theirs
UPDATE t SET j = '{"a": 3}';
-- expect
conflict row t 1
