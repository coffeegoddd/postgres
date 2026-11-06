-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1);
-- ours
ALTER TABLE t ADD COLUMN c1 varchar(100) DEFAULT 'hello';
INSERT INTO t VALUES (2, 'hi');
-- theirs
INSERT INTO t VALUES (3);
-- expect
clean
check SELECT string_agg(c1, ',' ORDER BY pk) FROM rt.t => hello,hi,hello
