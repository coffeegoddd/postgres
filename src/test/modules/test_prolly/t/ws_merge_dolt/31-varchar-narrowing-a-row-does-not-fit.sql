-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10));
INSERT INTO t VALUES (1, '123');
-- ours
ALTER TABLE t ALTER COLUMN col1 TYPE varchar(5);
-- theirs
INSERT INTO t VALUES (3, '1234567');
-- expect
conflict row t 3
