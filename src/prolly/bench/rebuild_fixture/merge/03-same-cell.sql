-- ours
UPDATE rt.acct SET name = 'o' WHERE id = 5;
-- theirs
UPDATE rt.acct SET name = 't' WHERE id = 5;
-- expect
conflict row acct 5
