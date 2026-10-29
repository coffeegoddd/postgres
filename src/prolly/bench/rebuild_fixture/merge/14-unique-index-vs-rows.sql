-- ours
CREATE UNIQUE INDEX acct_name ON rt.acct (name);
-- theirs
UPDATE rt.acct SET name = 'n1' WHERE id = 2;
-- expect
violation acct_name
