-- Rows after the column changes, drawn from the altered identities.
SET search_path = rt, public;
INSERT INTO item (a, b) SELECT g, 'last' || g FROM generate_series(1, 5) g;
UPDATE byd SET sq = -1 WHERE id = 1;
INSERT INTO byd (a) VALUES (12), (13);
UPDATE item SET a = NULL WHERE id = 105;
