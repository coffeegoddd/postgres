-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t SELECT 1, jsonb_object_agg('k' || g, g) FROM generate_series(1, 5000) g;
-- ours
UPDATE t SET j = j - 'k7' || '{"o": 1}';
-- theirs
UPDATE t SET j = j || '{"k9": 0}';
-- expect
clean
check SELECT j ->> 'o', j ->> 'k7', j ->> 'k9', j ->> 'k5000' FROM rt.t => 1||0|5000
