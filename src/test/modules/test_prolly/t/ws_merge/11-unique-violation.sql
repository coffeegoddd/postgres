-- ours
INSERT INTO rt.acct VALUES (5001, 'dup@x', 'a', 1);
-- theirs
INSERT INTO rt.acct VALUES (5002, 'dup@x', 'b', 1);
-- expect
violation acct_email_key acct 5001
violation acct_email_key acct 5002
-- resolve
UPDATE rt.acct SET email = 'dup2@x' WHERE id = 5002;
-- after
check SELECT contype FROM pg_constraint WHERE conname = 'acct_email_key' => u
check SELECT count(*) FROM prolly.violations => 0
