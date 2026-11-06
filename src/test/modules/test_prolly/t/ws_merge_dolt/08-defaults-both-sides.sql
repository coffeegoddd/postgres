-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1), (2);
-- ours
ALTER TABLE t ADD COLUMN col2 varchar(100) DEFAULT 'abc';
INSERT INTO t VALUES (3, '300'), (4, '400');
-- theirs
ALTER TABLE t ADD COLUMN col1 int DEFAULT 101;
INSERT INTO t VALUES (5, 50), (6, 60);
-- expect
clean
check SELECT string_agg(col1 || ':' || col2, ',' ORDER BY pk) FROM rt.t => 101:abc,101:abc,101:300,101:400,50:abc,60:abc
