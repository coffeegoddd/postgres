-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, (repeat('{"a": ', 300) || '{}' || repeat('}', 300))::jsonb);
-- ours
UPDATE t SET j = (repeat('{"a": ', 300) || '{"o": 1}' || repeat('}', 300))::jsonb;
-- theirs
UPDATE t SET j = (repeat('{"a": ', 300) || '{"t": 2}' || repeat('}', 300))::jsonb;
-- expect
clean
check SELECT j #>> (SELECT array_agg('a'::text) FROM generate_series(1, 300)) FROM rt.t => {"o": 1, "t": 2}
