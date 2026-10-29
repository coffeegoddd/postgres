-- ours
INSERT INTO rt.acct VALUES (5001, 'dup@x', 'a', 1);
-- theirs
INSERT INTO rt.acct VALUES (5002, 'dup@x', 'b', 1);
-- expect
violation acct_email_key
