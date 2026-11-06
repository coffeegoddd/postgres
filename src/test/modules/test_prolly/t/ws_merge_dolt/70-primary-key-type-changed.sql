-- base
CREATE TABLE t (pk1 bigint, pk2 bigint, PRIMARY KEY (pk1, pk2));
-- ours
INSERT INTO t VALUES (1, 1);
-- theirs
ALTER TABLE t ALTER COLUMN pk2 TYPE smallint;
INSERT INTO t VALUES (2, 2);
-- expect
conflict key t
