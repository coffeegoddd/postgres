-- base
CREATE TABLE t (pk int PRIMARY KEY, cents integer);
INSERT INTO t VALUES (1, 100);
-- ours
ALTER TABLE t ALTER COLUMN cents TYPE numeric(10,2) USING cents / 100.0;
-- theirs
INSERT INTO t VALUES (2, 250);
-- expect
conflict column t.cents
