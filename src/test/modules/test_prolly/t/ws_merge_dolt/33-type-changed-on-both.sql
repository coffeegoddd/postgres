-- base
CREATE TABLE t (i int PRIMARY KEY, j int);
-- ours
ALTER TABLE t ALTER COLUMN j TYPE varchar(100);
-- theirs
ALTER TABLE t ALTER COLUMN j TYPE real;
-- expect
conflict column t.j
