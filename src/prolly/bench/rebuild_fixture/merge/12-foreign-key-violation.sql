-- ours
DELETE FROM rt.item WHERE acct = 9;
DELETE FROM rt.acct WHERE id = 9;
-- theirs
INSERT INTO rt.item VALUES (9001, 9, 1, NULL);
-- expect
violation item_acct_fkey
