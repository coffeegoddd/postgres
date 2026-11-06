-- base
CREATE TABLE xyz (x int PRIMARY KEY, y int, z int);
CREATE INDEX y_idx ON xyz (y);
CREATE INDEX z_idx ON xyz (z);
INSERT INTO xyz VALUES (0, 0, 0), (1, 1, 1);
-- ours
INSERT INTO xyz VALUES (3, 3, 3);
UPDATE xyz SET z = 5 WHERE z = 1;
-- theirs
INSERT INTO xyz VALUES (4, 4, 4);
UPDATE xyz SET y = 2 WHERE z = 1;
-- expect
clean
check SELECT string_agg(y || ':' || z, ',' ORDER BY x) FROM rt.xyz => 0:0,2:5,3:3,4:4
