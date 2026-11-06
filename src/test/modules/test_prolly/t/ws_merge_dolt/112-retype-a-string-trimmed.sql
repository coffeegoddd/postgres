-- base
CREATE TABLE t (pk int PRIMARY KEY, v varchar(10));
-- ours
ALTER TABLE t ALTER COLUMN v TYPE varchar(3);
-- theirs
INSERT INTO t VALUES (1, 'ab      ');
-- expect
conflict row t 1
