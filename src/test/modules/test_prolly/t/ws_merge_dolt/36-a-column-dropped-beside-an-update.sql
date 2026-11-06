-- base
CREATE TABLE t (id int PRIMARY KEY, a int, b char(20), c char(20));
CREATE INDEX t_c ON t (c);
INSERT INTO t VALUES (1, 2, '3', '4');
-- ours
ALTER TABLE t DROP COLUMN b;
-- theirs
UPDATE t SET a = 2;
-- expect
clean
