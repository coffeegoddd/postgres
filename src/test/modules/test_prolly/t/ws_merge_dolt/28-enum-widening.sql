-- base
CREATE TYPE en AS ENUM ('blue', 'green');
CREATE TABLE t (pk int PRIMARY KEY, col1 en);
INSERT INTO t VALUES (1, 'blue');
-- ours
ALTER TYPE en ADD VALUE 'red';
-- theirs
INSERT INTO t VALUES (2, 'green');
-- expect
clean
check SELECT string_agg(col1::text, ',' ORDER BY pk) FROM rt.t => blue,green
