-- base
CREATE TABLE t (i int);
ALTER TABLE t ADD CONSTRAINT c CHECK (i > 0);
-- ours
INSERT INTO t VALUES (1);
-- theirs
ALTER TABLE t DROP CONSTRAINT c;
-- expect
clean
check SELECT count(*) FROM rt.t => 1
