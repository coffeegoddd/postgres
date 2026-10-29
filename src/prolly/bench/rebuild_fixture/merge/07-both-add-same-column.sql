-- ours
ALTER TABLE rt.acct ADD COLUMN phone text;
UPDATE rt.acct SET phone = 'o' WHERE id = 1;
-- theirs
ALTER TABLE rt.acct ADD COLUMN phone text;
UPDATE rt.acct SET phone = 't' WHERE id = 2;
-- expect
clean
check SELECT string_agg(phone, ',' ORDER BY id) FROM rt.acct => o,t
