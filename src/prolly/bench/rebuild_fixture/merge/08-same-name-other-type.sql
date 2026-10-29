-- ours
ALTER TABLE rt.acct ADD COLUMN phone text;
-- theirs
ALTER TABLE rt.acct ADD COLUMN phone int;
-- expect
conflict column-name acct.phone
