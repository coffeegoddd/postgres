-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{}');
-- ours
UPDATE t SET j = '{"a": 1}';
-- theirs
UPDATE t SET j = '{"b": 2}';
-- expect
conflict row t 1
