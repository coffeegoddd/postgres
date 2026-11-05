-- ours
ALTER TABLE rt.acct ADD COLUMN phone text;
UPDATE rt.acct SET phone = 'p' WHERE id <= 3;
INSERT INTO rt.acct VALUES (2000, 'new@x', 'new', 1, 'p');
-- theirs
ALTER TABLE rt.acct ADD COLUMN tier int DEFAULT 1;
-- expect
clean
check SELECT count(*), sum(tier) FROM rt.acct => 1001|1001
check SELECT phone, tier FROM rt.acct WHERE id = 2 => p|1
check SELECT phone, tier FROM rt.acct WHERE id = 2000 => p|1
