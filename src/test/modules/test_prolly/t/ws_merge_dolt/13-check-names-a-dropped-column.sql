-- base
CREATE TABLE t (id int PRIMARY KEY, coach int);
-- ours
ALTER TABLE t ADD CONSTRAINT chk_coach CHECK (coach <= 255);
-- theirs
ALTER TABLE t DROP COLUMN coach;
-- expect
conflict table-ddl t
