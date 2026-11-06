-- base
CREATE TABLE xyz (x int PRIMARY KEY, y int, z int);
CREATE INDEX y_idx ON xyz (y);
CREATE INDEX z_idx ON xyz (z);
INSERT INTO xyz VALUES (0, 0, 0), (1, 1, 1), (2, 2, 2), (3, 3, 3);
-- ours
DELETE FROM xyz WHERE y = 3;
-- theirs
DELETE FROM xyz WHERE y = 1;
-- expect
clean
check SELECT string_agg(y::text, ',' ORDER BY x) FROM rt.xyz => 0,2
