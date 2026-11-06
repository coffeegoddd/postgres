-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": 2}');
-- ours
UPDATE t SET j = '{"a": 1.0}';
-- theirs
UPDATE t SET j = '{"a": 1}';
-- expect
conflict row t 1
