-- Ours replaces the expression (a rewrite: every row's total changes);
-- theirs adds a row and changes a source of another under the old one.
-- Both of theirs are computed under the merged expression.
-- ours
ALTER TABLE rt.item ALTER COLUMN total SET EXPRESSION AS (a - b);
-- theirs
INSERT INTO rt.item VALUES (600, 50, 8);
UPDATE rt.item SET b = 40 WHERE id = 7;
-- expect
clean
check SELECT total FROM rt.item WHERE id = 600 => 42
check SELECT total FROM rt.item WHERE id = 7 => -33
check SELECT total FROM rt.item WHERE id = 8 => 7
