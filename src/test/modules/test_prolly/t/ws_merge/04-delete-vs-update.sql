-- ours
DELETE FROM rt.item WHERE id = 7;
-- theirs
UPDATE rt.item SET qty = 5 WHERE id = 7;
-- expect
conflict row item 7
