-- base
CREATE TABLE z (id int);
-- ours
CREATE TABLE t (pk int PRIMARY KEY, col1 int);
ALTER TABLE t ALTER COLUMN col1 TYPE varchar(255);
INSERT INTO t VALUES (1, 'one'), (2, 'two');
-- theirs
CREATE TABLE t (pk int PRIMARY KEY, col1 varchar(255));
INSERT INTO t VALUES (3, 'three');
-- expect
conflict table t
