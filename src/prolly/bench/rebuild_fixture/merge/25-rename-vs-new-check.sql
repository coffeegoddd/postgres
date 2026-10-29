-- ours
ALTER TABLE rt.acct RENAME COLUMN score TO points;
-- theirs
ALTER TABLE rt.acct ADD CONSTRAINT score_cap CHECK (score < 100000);
CREATE TRIGGER acct_touch BEFORE UPDATE OF score ON rt.acct
	FOR EACH ROW EXECUTE FUNCTION suppress_redundant_updates_trigger();
-- expect
clean
check SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'score_cap' => CHECK ((points < 100000))
check SELECT pg_get_triggerdef(oid) LIKE '%UPDATE OF points%' FROM pg_trigger WHERE tgname = 'acct_touch' => t
