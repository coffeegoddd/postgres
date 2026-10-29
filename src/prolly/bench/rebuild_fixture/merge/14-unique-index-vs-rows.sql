-- ours
CREATE UNIQUE INDEX acct_name ON rt.acct (name);
-- theirs
UPDATE rt.acct SET name = 'n1' WHERE id = 2;
-- expect
violation acct_name acct 1
violation acct_name acct 2
-- resolve
UPDATE rt.acct SET name = 'n2' WHERE id = 2;
-- after
check SELECT indisunique FROM pg_index WHERE indexrelid = 'rt.acct_name'::regclass => t
