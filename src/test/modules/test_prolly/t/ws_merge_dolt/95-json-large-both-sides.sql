-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t SELECT 1, jsonb_build_object('big', repeat('x', 5000), 'a', 1);
-- ours
UPDATE t SET j = j || '{"o": 1}';
-- theirs
UPDATE t SET j = j || '{"t": 2}';
-- expect
clean
check SELECT j ->> 'o', j ->> 't', length(j ->> 'big') FROM rt.t => 1|2|5000
