-- The merged values make the expression fail (a division by zero neither
-- side wrote): the row cannot be merged.
-- ours
ALTER TABLE rt.item ALTER COLUMN total SET EXPRESSION AS (a / b);
UPDATE rt.item SET a = 0 WHERE id = 5;
-- theirs
UPDATE rt.item SET b = 0 WHERE id = 5;
-- expect
conflict generated item 5
