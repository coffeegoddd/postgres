-- base
CREATE TABLE t (a int);
INSERT INTO t VALUES (1), (1), (2);
-- ours
DELETE FROM t WHERE ctid = (SELECT min(ctid) FROM t WHERE a = 1);
-- theirs
DELETE FROM t WHERE ctid = (SELECT min(ctid) FROM t WHERE a = 1);
-- expect
clean
check SELECT count(*) FROM rt.t WHERE a = 1 => 0
