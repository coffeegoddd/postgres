-- Rows change: a generated column's sources updated, rows deleted, more
-- drawn from the identities.
SET search_path = rt, public;
UPDATE item SET a = a + 1000 WHERE id % 3 = 0;
UPDATE item SET b = NULL WHERE id % 7 = 0;
DELETE FROM item WHERE id BETWEEN 300 AND 400;
INSERT INTO item (a, b) SELECT g, 'more' || g FROM generate_series(1, 20) g;
INSERT INTO byd (a) VALUES (9), (10);
DELETE FROM bag WHERE x = 2;
INSERT INTO bag VALUES (1), (3);
