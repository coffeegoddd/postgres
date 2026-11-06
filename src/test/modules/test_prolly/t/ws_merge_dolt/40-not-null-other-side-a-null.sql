-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, NULL), (2, NULL);
-- ours
UPDATE t SET col1 = 0 WHERE col1 IS NULL;
ALTER TABLE t ALTER COLUMN col1 SET NOT NULL;
-- theirs
INSERT INTO t VALUES (3, NULL);
-- expect
violation not null t t_col1_not_null 3
