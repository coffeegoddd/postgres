-- Ours adds a stored generated column; theirs adds rows and changes one
-- without it.  The merge computes it for theirs; ours' rows keep theirs.
-- ours
ALTER TABLE rt.item ADD COLUMN twice int GENERATED ALWAYS AS (a * 2) STORED;
-- theirs
INSERT INTO rt.item VALUES (700, 21, 0);
UPDATE rt.item SET a = 1000 WHERE id = 3;
INSERT INTO rt.bag VALUES (9);
-- expect
clean
check SELECT twice FROM rt.item WHERE id = 700 => 42
check SELECT twice FROM rt.item WHERE id = 3 => 2000
check SELECT twice FROM rt.item WHERE id = 4 => 8
check SELECT string_agg(x || ':' || gx, ',' ORDER BY x) FROM rt.bag => 1:2,2:4,9:18
