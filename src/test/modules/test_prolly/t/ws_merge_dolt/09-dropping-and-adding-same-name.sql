-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));
INSERT INTO t VALUES (1, 10, '100'), (2, 20, '200');
-- ours
ALTER TABLE t DROP COLUMN col1; ALTER TABLE t ADD COLUMN col1 int;
INSERT INTO t (pk, col1, col2) VALUES (3, 30, '300'), (4, 40, '400');
-- theirs
INSERT INTO t VALUES (5, 50, '500'), (6, 60, '600');
-- expect
clean
check SELECT string_agg(pk || ':' || coalesce(col1::text, 'null'), ',' ORDER BY pk) FROM rt.t => 1:null,2:null,3:30,4:40,5:50,6:60
