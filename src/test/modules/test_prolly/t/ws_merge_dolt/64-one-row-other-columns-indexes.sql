-- base
CREATE TABLE xyz (x int PRIMARY KEY, y int, z int);
CREATE INDEX y_idx ON xyz (y);
CREATE INDEX z_idx ON xyz (z);
INSERT INTO xyz VALUES (0, 0, 0), (1, 1, 1);
-- ours
UPDATE xyz SET z = 2 WHERE z = 1;
-- theirs
UPDATE xyz SET y = 2 WHERE z = 1;
-- expect
clean
check SELECT string_agg(y || ':' || z, ',' ORDER BY x) FROM rt.xyz => 0:0,2:2
