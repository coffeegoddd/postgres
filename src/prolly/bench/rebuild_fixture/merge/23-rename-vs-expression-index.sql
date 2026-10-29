-- ours
ALTER TABLE rt.acct RENAME COLUMN email TO mail;
-- theirs
CREATE INDEX acct_lower ON rt.acct (lower(email)) WHERE score > 10;
-- expect
clean
check SELECT indexdef FROM pg_indexes WHERE indexname = 'acct_lower' => CREATE INDEX acct_lower ON rt.acct USING btree (lower(mail)) WHERE (score > 10)
