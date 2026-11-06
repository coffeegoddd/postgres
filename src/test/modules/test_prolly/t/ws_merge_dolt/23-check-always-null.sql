-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT 'hello');
INSERT INTO t VALUES (1, 'hi');
-- ours
ALTER TABLE t ADD CONSTRAINT c1 CHECK (NULL = NULL);
-- theirs
INSERT INTO t VALUES (2, DEFAULT);
-- expect
clean
