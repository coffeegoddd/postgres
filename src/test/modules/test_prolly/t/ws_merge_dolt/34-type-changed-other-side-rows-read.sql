-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 10), (2, 20);
-- ours
ALTER TABLE t ALTER COLUMN col1 TYPE varchar(100);
INSERT INTO t VALUES (3, 'thirty'), (4, 'forty');
-- theirs
INSERT INTO t VALUES (5, 50), (6, 60);
-- expect
clean
check SELECT string_agg(col1, ',' ORDER BY pk) FROM rt.t => 10,20,thirty,forty,50,60
