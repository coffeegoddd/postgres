-- base
CREATE TABLE t (i int);
-- ours
INSERT INTO t VALUES (1);
-- theirs
ALTER TABLE t ADD CONSTRAINT c CHECK (i < 10);
-- expect
clean
check SELECT count(*) FROM rt.t => 1
