-- ours
INSERT INTO rt.log VALUES (1, 'm1');
-- theirs
INSERT INTO rt.log VALUES (1, 'm1');
DELETE FROM rt.log WHERE at = 2;
-- expect
clean
check SELECT count(*) FROM rt.log WHERE at = 1 => 3
check SELECT count(*) FROM rt.log WHERE at = 2 => 0
