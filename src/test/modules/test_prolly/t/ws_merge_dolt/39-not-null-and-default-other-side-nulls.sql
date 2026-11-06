-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
INSERT INTO t VALUES (1, NULL), (2, NULL);
-- ours
UPDATE t SET col1 = 9999 WHERE col1 IS NULL;
ALTER TABLE t ALTER COLUMN col1 SET DEFAULT 9999;
ALTER TABLE t ALTER COLUMN col1 SET NOT NULL;
INSERT INTO t VALUES (3, 30), (4, 40);
-- theirs
INSERT INTO t VALUES (5, NULL), (6, NULL);
-- expect
violation not null t t_col1_not_null 5
violation not null t t_col1_not_null 6
