-- ours
ALTER TABLE rt.acct ADD CONSTRAINT score_small CHECK (score < 5000);
-- theirs
UPDATE rt.acct SET score = 6000 WHERE id = 1;
-- expect
violation score_small acct 1
-- resolve
UPDATE rt.acct SET score = 10 WHERE id = 1;
-- after
check SELECT convalidated FROM pg_constraint WHERE conname = 'score_small' => t
