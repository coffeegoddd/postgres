-- The triggers themselves change: enable states (ALWAYS to REPLICA, a
-- statement trigger off, a REPLICA rule to ALWAYS), one dropped, one
-- added, a column added.  These are versioned like any other object.
SET search_path = rt, public;
ALTER TABLE item ADD COLUMN extra int DEFAULT 7;
ALTER TABLE item ENABLE REPLICA TRIGGER item_log_always;
ALTER TABLE item DISABLE TRIGGER item_count;
ALTER TABLE bag ENABLE ALWAYS RULE bag_rule;
DROP TRIGGER item_truncate ON item;
CREATE TRIGGER ident_bump BEFORE INSERT ON ident
	FOR EACH ROW EXECUTE FUNCTION log_row();
ALTER TABLE ident ENABLE ALWAYS TRIGGER ident_bump;
INSERT INTO item (id, v, note) VALUES (300, 3, 'after ddl');
