-- base
CREATE TABLE z (id int);
-- ours
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 1);
-- theirs
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (2, 2);
-- expect
clean
check SELECT string_agg(pk || ':' || col1, ',' ORDER BY pk) FROM rt.t => 1:1,2:2
