-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 int, CONSTRAINT c1 CHECK (col1 != col2));
INSERT INTO t VALUES (1, 2, 3);
-- ours
UPDATE t SET col2 = 4;
-- theirs
UPDATE t SET col1 = 4;
-- expect
violation check t c1 1
