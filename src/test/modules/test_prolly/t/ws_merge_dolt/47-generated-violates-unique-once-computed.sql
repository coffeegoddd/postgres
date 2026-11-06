-- base
CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED, CONSTRAINT ug UNIQUE (g));
INSERT INTO t (id, a, b) VALUES (1, 0, 0), (2, 5, 5);
-- ours
UPDATE t SET a = 4 WHERE id = 1;
-- theirs
UPDATE t SET b = 6 WHERE id = 1;
-- expect
violation unique t ug 1
violation unique t ug 2
check SELECT g FROM rt.t WHERE id = 1 => 10
