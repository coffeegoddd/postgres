-- base
CREATE TABLE t (pk int PRIMARY KEY, c1 varchar(100));
INSERT INTO t VALUES (1, 'one');
ALTER TABLE t ADD CONSTRAINT check1 CHECK (c1 IN ('one', 'two'));
-- ours
ALTER TABLE t DROP CONSTRAINT check1;
ALTER TABLE t ADD CONSTRAINT check1 CHECK (c1 IN ('one', 'two', 'three'));
INSERT INTO t VALUES (3, 'three');
-- theirs
INSERT INTO t VALUES (2, 'two');
-- expect
clean
check SELECT string_agg(c1, ',' ORDER BY pk) FROM rt.t => one,two,three
