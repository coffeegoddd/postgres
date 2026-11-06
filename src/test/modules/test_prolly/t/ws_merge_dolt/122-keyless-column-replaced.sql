-- base
CREATE TABLE t (n int, col1 int);
INSERT INTO t VALUES (1, 10);
-- ours
ALTER TABLE t ADD COLUMN col2 int;
UPDATE t SET col2 = col1 * 10;
ALTER TABLE t DROP COLUMN col1;
ALTER TABLE t RENAME COLUMN col2 TO col1;
-- theirs
INSERT INTO t VALUES (2, 20);
-- expect
conflict column t.col1
