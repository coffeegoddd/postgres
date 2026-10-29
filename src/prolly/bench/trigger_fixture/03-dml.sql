-- More rows through the triggers as they now stand, and a row locked so
-- that a move back must delete it past its guard.
SET search_path = rt, public;
UPDATE item SET v = v * 2, extra = id WHERE id % 5 = 0;
UPDATE item SET locked = true WHERE id = 300;
INSERT INTO item (id, v, note, locked) VALUES (301, 1, 'locked late', true);
INSERT INTO ident (v) SELECT g FROM generate_series(600, 610) g;
INSERT INTO bag VALUES (9, 'nine'), (9, 'nine');
