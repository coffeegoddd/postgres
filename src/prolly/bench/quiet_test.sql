-- prolly.quiet() and prolly.unquiet(), directly: what they turn off, that
-- everything comes back as it was, and that an object dropped or made again
-- between them keeps what the operation left it.  Run in a database that
-- has prolly_tags.sql; every check raises on failure.
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
DROP SCHEMA IF EXISTS qt CASCADE;
DROP EVENT TRIGGER IF EXISTS qt_evt_always;
DROP EVENT TRIGGER IF EXISTS qt_evt_origin;
CREATE SCHEMA qt;
CREATE TABLE qt.t (id int PRIMARY KEY, v int);
CREATE TABLE qt.log (what text);
CREATE FUNCTION qt.trg() RETURNS trigger LANGUAGE plpgsql AS
	$$BEGIN INSERT INTO qt.log VALUES (TG_NAME); RETURN NEW; END$$;
CREATE FUNCTION qt.evt() RETURNS event_trigger LANGUAGE plpgsql AS
	$$BEGIN INSERT INTO qt.log VALUES ('event ' || tg_tag); END$$;
CREATE TRIGGER t_always AFTER INSERT ON qt.t FOR EACH ROW EXECUTE FUNCTION qt.trg();
ALTER TABLE qt.t ENABLE ALWAYS TRIGGER t_always;
CREATE TRIGGER t_replica AFTER INSERT ON qt.t FOR EACH ROW EXECUTE FUNCTION qt.trg();
ALTER TABLE qt.t ENABLE REPLICA TRIGGER t_replica;
CREATE TRIGGER t_origin AFTER INSERT ON qt.t FOR EACH ROW EXECUTE FUNCTION qt.trg();
CREATE TRIGGER t_off AFTER INSERT ON qt.t FOR EACH ROW EXECUTE FUNCTION qt.trg();
ALTER TABLE qt.t DISABLE TRIGGER t_off;
CREATE RULE r_always AS ON UPDATE TO qt.t DO ALSO INSERT INTO qt.log VALUES ('rule');
ALTER TABLE qt.t ENABLE ALWAYS RULE r_always;
CREATE EVENT TRIGGER qt_evt_always ON ddl_command_end EXECUTE FUNCTION qt.evt();
ALTER EVENT TRIGGER qt_evt_always ENABLE ALWAYS;
CREATE EVENT TRIGGER qt_evt_origin ON ddl_command_end EXECUTE FUNCTION qt.evt();

CREATE FUNCTION qt.states() RETURNS text LANGUAGE sql AS $$
	SELECT (SELECT string_agg(tgname || '=' || tgenabled::text, ',' ORDER BY tgname) FROM pg_trigger
			WHERE tgrelid = 'qt.t'::regclass AND NOT tgisinternal) || ' ' ||
		   (SELECT string_agg(rulename || '=' || ev_enabled::text, ',') FROM pg_rewrite
			WHERE ev_class = 'qt.t'::regclass) || ' ' ||
		   (SELECT string_agg(evtname || '=' || evtenabled::text, ',' ORDER BY evtname) FROM pg_event_trigger
			WHERE evtname LIKE 'qt\_%')
$$;
CREATE FUNCTION qt.expect(got text, want text, what text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	IF got IS DISTINCT FROM want THEN
		RAISE EXCEPTION '%: got %, want %', what, got, want;
	END IF;
END $$;

-- 1. what is turned off, and that it all comes back
TRUNCATE qt.log;
SELECT qt.expect(qt.states(),
	't_always=A,t_off=D,t_origin=O,t_replica=R r_always=A qt_evt_always=A,qt_evt_origin=O', 'before');
BEGIN;
SELECT prolly.quiet();
SELECT qt.expect(current_setting('session_replication_role'), 'replica', 'replica mode');
SELECT qt.expect(qt.states(),
	't_always=D,t_off=D,t_origin=O,t_replica=D r_always=D qt_evt_always=D,qt_evt_origin=O', 'quieted');
-- nothing of the user's runs: rows, a rule's statement, DDL
INSERT INTO qt.t VALUES (1, 1);
UPDATE qt.t SET v = 2;
CREATE TABLE qt.made_while_quiet (x int);
SELECT qt.expect((SELECT count(*)::text FROM qt.log), '0', 'nothing logged while quiet');
SELECT prolly.unquiet();
SELECT qt.expect(qt.states(),
	't_always=A,t_off=D,t_origin=O,t_replica=R r_always=A qt_evt_always=A,qt_evt_origin=O', 'unquieted');
SELECT qt.expect((SELECT count(*)::text FROM prolly.quieted), '0', 'nothing left recorded');
COMMIT;
SELECT qt.expect(current_setting('session_replication_role'), 'origin', 'replica mode ends with the transaction');

-- 2. made again between them, under the same name: it keeps the state it was made with
TRUNCATE qt.log;
BEGIN;
SELECT prolly.quiet();
DROP TRIGGER t_always ON qt.t;
CREATE TRIGGER t_always AFTER INSERT ON qt.t FOR EACH ROW EXECUTE FUNCTION qt.trg();
ALTER TABLE qt.t ENABLE REPLICA TRIGGER t_always;
DROP RULE r_always ON qt.t;
CREATE RULE r_always AS ON UPDATE TO qt.t DO ALSO INSERT INTO qt.log VALUES ('rule');
-- set by the operation on the same object: its state stands
ALTER TABLE qt.t ENABLE TRIGGER t_replica;
SELECT prolly.unquiet();
SELECT qt.expect(qt.states(),
	't_always=R,t_off=D,t_origin=O,t_replica=O r_always=O qt_evt_always=A,qt_evt_origin=O', 'remade');
COMMIT;

-- 3. dropped between them: it stays dropped, and nothing fails
BEGIN;
SELECT prolly.quiet();
DROP TRIGGER t_replica ON qt.t;
DROP EVENT TRIGGER qt_evt_always;
SELECT prolly.unquiet();
COMMIT;
SELECT qt.expect(qt.states(), 't_always=R,t_off=D,t_origin=O r_always=O qt_evt_origin=O', 'dropped');

-- 4. rolled back: the catalog is as it was
CREATE EVENT TRIGGER qt_evt_always ON ddl_command_end EXECUTE FUNCTION qt.evt();
ALTER EVENT TRIGGER qt_evt_always ENABLE ALWAYS;
BEGIN;
SELECT prolly.quiet();
ROLLBACK;
SELECT qt.expect(qt.states(), 't_always=R,t_off=D,t_origin=O r_always=O qt_evt_always=A,qt_evt_origin=O',
	'rolled back');

DROP EVENT TRIGGER qt_evt_always;
DROP EVENT TRIGGER qt_evt_origin;
DROP SCHEMA qt CASCADE;
\echo quiet_test: all passed
