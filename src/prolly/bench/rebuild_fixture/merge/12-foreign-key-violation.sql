-- ours
DELETE FROM rt.item WHERE acct = 9;
DELETE FROM rt.acct WHERE id = 9;
-- theirs
INSERT INTO rt.item VALUES (9001, 9, 1, NULL);
-- expect
violation item_acct_fkey item 9001
-- resolve
INSERT INTO rt.acct VALUES (9, 'u9@x', 'n9', 9);
-- after
check SELECT convalidated FROM pg_constraint WHERE conname = 'item_acct_fkey' => t
