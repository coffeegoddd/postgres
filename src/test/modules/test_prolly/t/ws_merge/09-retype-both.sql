-- ours
ALTER TABLE rt.acct ALTER COLUMN score TYPE bigint;
-- theirs
ALTER TABLE rt.acct ALTER COLUMN score TYPE numeric;
-- expect
conflict column acct.score
