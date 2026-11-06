-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(10));
INSERT INTO t VALUES (1, '123');
-- ours
ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100);
INSERT INTO t VALUES (2, '12345678901234567890');
-- theirs
INSERT INTO t VALUES (3, '321');
-- expect
clean
check SELECT string_agg(col1, ',' ORDER BY pk) FROM rt.t => 123,12345678901234567890,321
