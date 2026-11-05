-- Each side changes a different source of the same rows: the merged row
-- takes both, and its generated value is computed from them.  Neither
-- side's total is right for it, and the totals never conflict.
-- ours
UPDATE rt.item SET a = a + 100 WHERE id <= 10;
-- theirs
UPDATE rt.item SET b = 5 WHERE id <= 10;
INSERT INTO rt.item VALUES (500, 2, 3);
-- expect
clean
check SELECT total FROM rt.item WHERE id = 1 => 1015
check SELECT sum(total) FROM rt.item WHERE id <= 10 => 10600
check SELECT total FROM rt.item WHERE id = 500 => 23
check SELECT total FROM rt.item WHERE id = 50 => 501
