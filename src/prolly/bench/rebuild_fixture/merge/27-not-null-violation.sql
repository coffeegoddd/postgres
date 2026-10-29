-- ours
ALTER TABLE rt.acct ALTER COLUMN name SET NOT NULL;
-- theirs
UPDATE rt.acct SET name = NULL WHERE id IN (3, 4);
-- expect
violation acct_name_not_null acct 3
violation acct_name_not_null acct 4
-- resolve
UPDATE rt.acct SET name = 'back' WHERE name IS NULL;
-- after
check SELECT attnotnull FROM pg_attribute WHERE attrelid = 'rt.acct'::regclass AND attname = 'name' => t
