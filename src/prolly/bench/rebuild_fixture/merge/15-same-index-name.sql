-- ours
CREATE INDEX x ON rt.acct (name);
-- theirs
CREATE INDEX x ON rt.acct (score);
-- expect
conflict object INDEX x
