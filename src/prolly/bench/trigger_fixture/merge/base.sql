-- The base every trigger merge branches from: rows written through user
-- code (triggers, a rule, a row rewriter), and an audit log they append to,
-- keyless so that both branches' entries merge as a multiset.
CREATE SCHEMA rt;
CREATE TABLE rt.audit (what text NOT NULL);
CREATE TABLE rt.item (id int PRIMARY KEY, v int NOT NULL, note text);
CREATE FUNCTION rt.log_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO rt.audit VALUES (TG_TABLE_NAME || ' ' || TG_OP || ' ' || TG_NAME);
	RETURN COALESCE(NEW, OLD);
END $$;
CREATE FUNCTION rt.bump() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	NEW.v := NEW.v + 1000;
	RETURN NEW;
END $$;
CREATE TRIGGER item_log AFTER INSERT OR UPDATE OR DELETE ON rt.item
	FOR EACH ROW EXECUTE FUNCTION rt.log_row();
ALTER TABLE rt.item ENABLE ALWAYS TRIGGER item_log;
CREATE TRIGGER item_bump BEFORE INSERT ON rt.item
	FOR EACH ROW EXECUTE FUNCTION rt.bump();
ALTER TABLE rt.item ENABLE ALWAYS TRIGGER item_bump;
CREATE RULE item_rule AS ON UPDATE TO rt.item
	DO ALSO INSERT INTO rt.audit VALUES ('rule ' || NEW.id);
ALTER TABLE rt.item ENABLE ALWAYS RULE item_rule;
INSERT INTO rt.item SELECT g, g, 'n' || g FROM generate_series(1, 100) g;
