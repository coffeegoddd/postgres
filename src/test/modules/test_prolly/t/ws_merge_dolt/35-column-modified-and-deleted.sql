-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
-- ours
ALTER TABLE t ALTER COLUMN col1 TYPE bigint;
-- theirs
ALTER TABLE t DROP COLUMN col1;
-- expect
conflict column t.col1
