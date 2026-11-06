-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100), col3 varchar(50), col4 varchar(20), UNIQUE (col2, pk));
INSERT INTO t VALUES (1, 10, '100', '1', '11'), (2, 20, '200', '2', '22');
CREATE INDEX idx1 ON t (col4, col1);
-- ours
ALTER TABLE t DROP COLUMN col3;
UPDATE t SET col1 = -100, col2 = '-100' WHERE pk = 1;
-- theirs
UPDATE t SET col1 = -1000 WHERE pk = 1;
-- expect
conflict row t 1
