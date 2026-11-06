-- base
CREATE TABLE t (pk int PRIMARY KEY, a int, b smallint, CONSTRAINT uq UNIQUE (a));
INSERT INTO t VALUES (1, 10, 1);
-- ours
ALTER TABLE t DROP CONSTRAINT uq;
ALTER TABLE t ADD CONSTRAINT uq UNIQUE (a, b);
-- theirs
INSERT INTO t VALUES (2, 20, 2);
-- expect
clean
