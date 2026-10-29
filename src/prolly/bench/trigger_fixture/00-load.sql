-- User code that must not run when version control writes: triggers of
-- every kind and enable state, rules, a trigger that rewrites rows, one
-- that refuses deletes, identity and generated columns, a keyless table.
-- When the user writes, it all runs, and what it does is committed.
CREATE SCHEMA rt;
SET search_path = rt, public;

CREATE TABLE audit (id bigserial PRIMARY KEY, what text NOT NULL);
CREATE TABLE counter (id int PRIMARY KEY, n int NOT NULL);
INSERT INTO counter VALUES (1, 0);
CREATE TABLE item (
	id int PRIMARY KEY,
	v int NOT NULL,
	note text,
	locked bool NOT NULL DEFAULT false,
	twice int GENERATED ALWAYS AS (v * 2) STORED
);
CREATE TABLE ident (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v int);
CREATE TABLE bag (x int, y text);
CREATE TABLE inbox (id int PRIMARY KEY, msg text);
CREATE TABLE outbox (id int PRIMARY KEY, msg text);

CREATE FUNCTION log_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO rt.audit (what) VALUES (TG_TABLE_NAME || ' ' || TG_OP || ' ' || TG_WHEN || ' ' ||
										TG_LEVEL || ' ' || TG_NAME);
	RETURN COALESCE(NEW, OLD);
END $$;
CREATE FUNCTION bump() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	NEW.v := NEW.v + 1000;
	NEW.note := coalesce(NEW.note, '') || ' bumped';
	RETURN NEW;
END $$;
CREATE FUNCTION count_up() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	UPDATE rt.counter SET n = n + 1 WHERE id = 1;
	RETURN NULL;
END $$;
CREATE FUNCTION guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF OLD.locked THEN
		RAISE EXCEPTION 'item % is locked', OLD.id;
	END IF;
	RETURN OLD;
END $$;

-- rows written before the rule that diverts them exists
INSERT INTO inbox SELECT g, CASE WHEN g % 3 = 0 THEN 'divert ' || g ELSE 'keep ' || g END
FROM generate_series(1, 30) g;

CREATE TRIGGER item_log_always AFTER INSERT OR UPDATE OR DELETE ON item
	FOR EACH ROW EXECUTE FUNCTION log_row();
ALTER TABLE item ENABLE ALWAYS TRIGGER item_log_always;
CREATE TRIGGER item_log_replica AFTER INSERT OR UPDATE OR DELETE ON item
	FOR EACH ROW EXECUTE FUNCTION log_row();
ALTER TABLE item ENABLE REPLICA TRIGGER item_log_replica;
CREATE TRIGGER item_log_origin AFTER INSERT OR UPDATE OR DELETE ON item
	FOR EACH ROW EXECUTE FUNCTION log_row();
CREATE TRIGGER item_bump BEFORE INSERT ON item
	FOR EACH ROW EXECUTE FUNCTION bump();
ALTER TABLE item ENABLE ALWAYS TRIGGER item_bump;
CREATE TRIGGER item_guard BEFORE DELETE ON item
	FOR EACH ROW EXECUTE FUNCTION guard();
ALTER TABLE item ENABLE ALWAYS TRIGGER item_guard;
CREATE TRIGGER item_count AFTER INSERT OR UPDATE OR DELETE ON item
	FOR EACH STATEMENT EXECUTE FUNCTION count_up();
ALTER TABLE item ENABLE ALWAYS TRIGGER item_count;
CREATE TRIGGER item_truncate AFTER TRUNCATE ON item
	FOR EACH STATEMENT EXECUTE FUNCTION log_row();
ALTER TABLE item ENABLE ALWAYS TRIGGER item_truncate;
CREATE CONSTRAINT TRIGGER item_deferred AFTER INSERT OR UPDATE ON item
	DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION log_row();
ALTER TABLE item ENABLE ALWAYS TRIGGER item_deferred;
CREATE TRIGGER ident_log AFTER INSERT OR UPDATE ON ident
	FOR EACH ROW EXECUTE FUNCTION log_row();
ALTER TABLE ident ENABLE ALWAYS TRIGGER ident_log;
CREATE TRIGGER bag_log AFTER INSERT OR DELETE ON bag
	FOR EACH ROW EXECUTE FUNCTION log_row();
ALTER TABLE bag ENABLE REPLICA TRIGGER bag_log;

CREATE RULE item_rule AS ON UPDATE TO item
	DO ALSO INSERT INTO audit (what) VALUES ('rule update ' || NEW.id);
ALTER TABLE item ENABLE ALWAYS RULE item_rule;
CREATE RULE bag_rule AS ON INSERT TO bag
	DO ALSO INSERT INTO audit (what) VALUES ('rule bag ' || NEW.x);
ALTER TABLE bag ENABLE REPLICA RULE bag_rule;
CREATE RULE inbox_divert AS ON INSERT TO inbox WHERE NEW.msg LIKE 'divert%'
	DO INSTEAD INSERT INTO outbox VALUES (NEW.id, NEW.msg);
ALTER TABLE inbox ENABLE ALWAYS RULE inbox_divert;

-- the user's writes: every trigger above that applies runs
INSERT INTO item (id, v, note, locked) SELECT g, g, 'n' || g, g <= 5 FROM generate_series(1, 200) g;
INSERT INTO ident (v) SELECT g FROM generate_series(1, 50) g;
INSERT INTO bag SELECT g % 7, 'y' || (g % 3) FROM generate_series(1, 60) g;
INSERT INTO inbox VALUES (100, 'divert late'), (101, 'keep late');
