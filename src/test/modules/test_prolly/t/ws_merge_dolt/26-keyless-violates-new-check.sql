-- base
CREATE TABLE t (c0 int, col0 varchar(100), col1 varchar(100) DEFAULT 'hello');
INSERT INTO t VALUES (1, 'adios', 'hi');
-- ours
INSERT INTO t VALUES (2, 'hola', DEFAULT);
-- theirs
ALTER TABLE t ADD CONSTRAINT c1 CHECK (col1 != 'hello');
-- expect
violation check t c1 (2,hola,hello)
