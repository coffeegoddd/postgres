-- base
CREATE TABLE t (pk int PRIMARY KEY, v timestamp);
-- ours
ALTER TABLE t ALTER COLUMN v TYPE timestamp(0);
-- theirs
INSERT INTO t VALUES (1, '2026-01-02 03:04:05.678');
-- expect
conflict row t 1
