-- base
CREATE TABLE t (pk int PRIMARY KEY, v numeric(8,4));
-- ours
ALTER TABLE t ALTER COLUMN v TYPE numeric(4,2);
-- theirs
INSERT INTO t VALUES (1, 1.2345);
-- expect
conflict row t 1
