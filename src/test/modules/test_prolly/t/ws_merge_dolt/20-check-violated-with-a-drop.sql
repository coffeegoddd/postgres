-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, col3 int, CONSTRAINT c1 CHECK (col2 != col3));
INSERT INTO t VALUES (1, 2, 3, -3);
-- ours
UPDATE t SET col2 = 100;
-- theirs
ALTER TABLE t DROP COLUMN col1;
UPDATE t SET col3 = 100;
-- expect
violation check t c1 1
