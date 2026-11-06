-- base
CREATE TABLE t (pk serial PRIMARY KEY, c0 int);
INSERT INTO t (c0) VALUES (1), (2);
-- ours
INSERT INTO t VALUES (4, 4), (5, 5);
-- theirs
INSERT INTO t (c0) VALUES (9);
-- expect
clean
check SELECT string_agg(pk || ':' || c0, ',' ORDER BY pk) FROM rt.t => 1:1,2:2,3:9,4:4,5:5
