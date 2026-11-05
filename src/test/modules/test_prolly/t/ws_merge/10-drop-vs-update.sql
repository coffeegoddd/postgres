-- ours
ALTER TABLE rt.acct DROP COLUMN name;
-- theirs
UPDATE rt.acct SET name = 't' WHERE id <= 5;
-- expect
conflict row acct 1
conflict row acct 2
conflict row acct 3
conflict row acct 4
conflict row acct 5
