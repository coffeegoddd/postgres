-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1), (2);
-- ours
ALTER TABLE t ADD COLUMN col2 varchar(100) GENERATED ALWAYS AS (pk::text || 'hello') VIRTUAL;
INSERT INTO t (pk) VALUES (3), (4);
-- theirs
ALTER TABLE t ADD COLUMN col1 int GENERATED ALWAYS AS (pk + 100) STORED;
INSERT INTO t (pk) VALUES (5), (6);
-- expect
clean
check SELECT string_agg(col1 || ':' || col2, ',' ORDER BY pk) FROM rt.t => 101:1hello,102:2hello,103:3hello,104:4hello,105:5hello,106:6hello
