-- ours
ALTER TABLE rt.acct DROP COLUMN name;
-- theirs
UPDATE rt.acct SET name = 't' WHERE id <= 5;
-- expect
clean
check SELECT count(*) FROM information_schema.columns WHERE table_schema = 'rt' AND table_name = 'acct' AND column_name = 'name' => 0
