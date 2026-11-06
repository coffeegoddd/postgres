-- base
CREATE TABLE t (pk int PRIMARY KEY, j jsonb);
INSERT INTO t VALUES (1, '[1]');
-- ours
UPDATE t SET j = '[1, 2]';
-- theirs
UPDATE t SET j = '[1, 3]';
-- expect
conflict row t 1
