-- ours
ALTER TABLE rt.acct ADD COLUMN phone text;
UPDATE rt.acct SET phone = 'o' WHERE id = 1;
-- theirs
ALTER TABLE rt.acct ADD COLUMN phone text;
UPDATE rt.acct SET phone = 't' WHERE id = 2;
-- expect
conflict row acct 1
conflict row acct 2
