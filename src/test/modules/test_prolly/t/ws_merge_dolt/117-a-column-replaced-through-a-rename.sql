-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, 10), (2, 20);
-- ours
INSERT INTO t VALUES (3, 30), (4, 40);
-- theirs
ALTER TABLE t ADD COLUMN col2 int;
UPDATE t SET col2 = col1 * 10;
ALTER TABLE t DROP COLUMN col1;
ALTER TABLE t RENAME COLUMN col2 TO col1;
-- expect
conflict column t.col1
