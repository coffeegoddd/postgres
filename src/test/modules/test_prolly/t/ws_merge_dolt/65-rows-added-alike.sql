-- base
CREATE TABLE xyz (x int PRIMARY KEY, y int, z int);
CREATE INDEX y_idx ON xyz (y);
CREATE INDEX z_idx ON xyz (z);
INSERT INTO xyz VALUES (0, 0, 0);
-- ours
INSERT INTO xyz VALUES (1, 1, 1);
-- theirs
INSERT INTO xyz VALUES (1, 1, 1), (2, 2, 2);
-- expect
clean
check SELECT string_agg(y::text, ',' ORDER BY x) FROM rt.xyz => 0,1,2
