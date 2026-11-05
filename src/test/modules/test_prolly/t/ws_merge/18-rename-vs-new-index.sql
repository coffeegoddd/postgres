-- ours
ALTER TABLE rt.acct RENAME COLUMN name TO full_name;
-- theirs
CREATE INDEX acct_name_idx ON rt.acct (name);
-- expect
clean
check SELECT indexdef FROM pg_indexes WHERE indexname = 'acct_name_idx' => CREATE INDEX acct_name_idx ON rt.acct USING btree (full_name)
