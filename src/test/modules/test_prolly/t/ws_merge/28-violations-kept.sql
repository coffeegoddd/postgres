-- Unresolved: the violations stay in the working set and its commits.
-- ours
ALTER TABLE rt.item ADD CONSTRAINT qty_small CHECK (qty < 20);
UPDATE rt.acct SET email = 'z@x' WHERE id = 1;
-- theirs
UPDATE rt.item SET qty = 50 WHERE id IN (10, 20);
INSERT INTO rt.acct VALUES (5001, 'z@x', 'x', 1);
-- expect
violation qty_small item 10
violation qty_small item 20
violation acct_email_key acct 1
violation acct_email_key acct 5001
