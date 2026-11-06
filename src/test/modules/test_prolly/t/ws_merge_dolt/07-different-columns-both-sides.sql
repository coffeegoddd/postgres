-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1), (2);
-- ours
ALTER TABLE t ADD COLUMN col2 varchar(100);
INSERT INTO t VALUES (3, '300'), (4, '400');
-- theirs
ALTER TABLE t ADD COLUMN col1 int;
INSERT INTO t VALUES (5, 50), (6, 60);
-- expect
clean
check SELECT count(*), count(col1), count(col2) FROM rt.t => 6|2|2
