-- base
CREATE TABLE t (pk int PRIMARY KEY, c0 varchar(20));
-- ours
ALTER TABLE t ALTER COLUMN c0 TYPE timestamp USING NULL;
-- theirs
ALTER TABLE t ALTER COLUMN c0 TYPE int USING NULL;
-- expect
conflict column t.c0
