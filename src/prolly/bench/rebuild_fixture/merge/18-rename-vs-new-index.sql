-- ours
ALTER TABLE rt.acct RENAME COLUMN name TO full_name;
-- theirs
CREATE INDEX acct_name_idx ON rt.acct (name);
-- expect
unapplied INDEX acct_name_idx
