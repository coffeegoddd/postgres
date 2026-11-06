-- base
CREATE TABLE t (f real);
-- ours
ALTER TABLE t ALTER COLUMN f SET DEFAULT '1.00';
-- theirs
ALTER TABLE t ALTER COLUMN f SET DEFAULT '1.000000000';
-- expect
clean
