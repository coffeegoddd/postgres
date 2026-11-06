-- base
CREATE TABLE t (pk int PRIMARY KEY, v double precision);
-- ours
ALTER TABLE t ALTER COLUMN v TYPE real;
-- theirs
INSERT INTO t VALUES (1, 1.23456789012345);
-- expect
conflict row t 1
