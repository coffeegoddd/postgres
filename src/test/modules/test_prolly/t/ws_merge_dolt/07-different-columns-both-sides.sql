-- base
CREATE TABLE t (pk int PRIMARY KEY);
INSERT INTO t VALUES (1), (2);
-- ours
ALTER TABLE t ADD COLUMN col2 varchar(100);
INSERT INTO t VALUES (3, '300'), (4, '400');
-- theirs
ALTER TABLE t ADD COLUMN col1 int;
INSERT INTO t VALUES (5, 50), (6, 60);
-- expect
clean
check SELECT string_agg(pk || ':' || coalesce(col1::text, 'null') || ':' || coalesce(col2, 'null'), ',' ORDER BY pk) FROM rt.t => 1:null:null,2:null:null,3:null:300,4:null:400,5:50:null,6:60:null
