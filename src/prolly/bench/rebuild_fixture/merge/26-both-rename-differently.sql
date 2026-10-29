-- ours
ALTER TABLE rt.acct RENAME COLUMN name TO a_name;
-- theirs
ALTER TABLE rt.acct RENAME COLUMN name TO b_name;
-- expect
conflict column acct.a_name
