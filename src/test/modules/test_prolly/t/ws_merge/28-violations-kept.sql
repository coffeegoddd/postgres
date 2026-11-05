-- Unresolved: the violations stay in the working set and its commits.
-- ours
ALTER TABLE rt.item ADD CONSTRAINT qty_small CHECK (qty < 20);
UPDATE rt.acct SET email = 'z@x' WHERE id = 1;
-- theirs
UPDATE rt.item SET qty = 50 WHERE id IN (10, 20);
INSERT INTO rt.acct VALUES (5001, 'z@x', 'x', 1);
-- expect
violation check item qty_small 10
violation check item qty_small 20
violation unique acct acct_email_key 1
violation unique acct acct_email_key 5001
