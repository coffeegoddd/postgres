-- base
CREATE TABLE t (a int);
INSERT INTO t VALUES (1);
-- ours
INSERT INTO t VALUES (2);
-- theirs
INSERT INTO t VALUES (2);
-- expect
clean
check SELECT count(*) FROM rt.t WHERE a = 2 => 2
