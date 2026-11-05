-- ours
ALTER TABLE rt.item ENABLE REPLICA TRIGGER item_log;
ALTER TABLE rt.item DISABLE RULE item_rule;
-- theirs
INSERT INTO rt.item VALUES (700, 3, 'theirs');
UPDATE rt.item SET v = v + 1 WHERE id <= 5;
-- expect
clean
check SELECT v FROM rt.item WHERE id = 700 => 1003
check SELECT count(*) FROM rt.audit => 111
check SELECT tgenabled FROM pg_trigger WHERE tgname = 'item_log' => R
check SELECT ev_enabled FROM pg_rewrite WHERE rulename = 'item_rule' => D
