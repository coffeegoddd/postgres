-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '{"a": {"x": 1}}');
-- ours
UPDATE t SET j = '{"a": 5}';
-- theirs
UPDATE t SET j = '{"a": {"x": 2}}';
-- expect
conflict row t 1
