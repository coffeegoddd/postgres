-- base
CREATE TABLE t (id int PRIMARY KEY, a int, b int, g int GENERATED ALWAYS AS (a + b) STORED, CONSTRAINT cg CHECK (g <= 6));
INSERT INTO t (id, a, b) VALUES (1, 0, 0);
-- ours
UPDATE t SET a = 4 WHERE id = 1;
-- theirs
UPDATE t SET b = 6 WHERE id = 1;
-- expect
violation check t cg 1
check SELECT g FROM rt.t => 10
