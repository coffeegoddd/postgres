-- ours
ALTER TABLE rt.acct RENAME COLUMN name TO full_name;
-- theirs
UPDATE rt.acct SET name = 't' WHERE id <= 5;
-- expect
clean
check SELECT count(*) FROM rt.acct WHERE full_name = 't' => 5
