-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb, g text GENERATED ALWAYS AS (j ->> 'a') STORED);
INSERT INTO t (pk, j) VALUES (1, '{"a": "x"}');
-- ours
UPDATE t SET j = '{"a": "y"}';
-- theirs
UPDATE t SET j = '{"a": "x", "b": 2}';
-- expect
clean
check SELECT j, g FROM rt.t => {"a": "y", "b": 2}|y
