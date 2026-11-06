-- base
CREATE TABLE t (f real);
-- ours
ALTER TABLE t ALTER COLUMN f SET DEFAULT 1.23;
-- theirs
ALTER TABLE t ALTER COLUMN f SET DEFAULT '1.23';
-- expect
conflict column t.f
