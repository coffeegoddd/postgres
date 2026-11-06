-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(100) DEFAULT 'hello');
-- ours
ALTER TABLE t ADD CONSTRAINT c1 CHECK (1 = 2);
-- theirs
INSERT INTO t VALUES (1, DEFAULT);
-- expect
violation check t c1 1
