-- ours
UPDATE rt.item SET note = 'o' WHERE id <= 10;
INSERT INTO rt.item VALUES (500, 1, 'ours');
-- theirs
UPDATE rt.item SET note = 't' WHERE id BETWEEN 51 AND 60;
INSERT INTO rt.item VALUES (600, 2, 'theirs');
DELETE FROM rt.item WHERE id = 99;
-- expect
clean
check SELECT v FROM rt.item WHERE id = 600 => 1002
check SELECT count(*) FROM rt.item WHERE note IN ('o', 't') => 20
check SELECT count(*) FROM rt.audit => 143
