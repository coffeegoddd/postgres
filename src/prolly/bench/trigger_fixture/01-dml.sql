-- Row changes through the user's code: updates, deletes (the locked row
-- unlocked first, so its guard lets it go), inserts, duplicate rows.
SET search_path = rt, public;
UPDATE item SET v = v + 1 WHERE id % 4 = 0;
UPDATE item SET locked = false WHERE id = 1;
DELETE FROM item WHERE id = 1 OR (id > 5 AND id % 9 = 0);
INSERT INTO item (id, v, note) SELECT g, g, 'later' FROM generate_series(201, 260) g;
UPDATE ident SET v = -v WHERE id % 5 = 0;
INSERT INTO ident (v) VALUES (500), (501);
DELETE FROM bag WHERE x = 3;
INSERT INTO bag SELECT 3, 'again' FROM generate_series(1, 4);
DELETE FROM inbox WHERE id % 6 = 0;
INSERT INTO inbox VALUES (102, 'keep 102');
