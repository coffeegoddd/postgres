-- An event trigger is shared by every branch, and may call a function in a
-- versioned schema: each branch's copy, as the session's branch has it.  The
-- trigger records no dependency on such a function, so a branch may change
-- or drop its own; where the session's branch hasn't it, the trigger is
-- skipped with a warning, and the command still runs.  A function a move
-- makes again (with another OID) is found by its name.  (In a database of
-- its own, with public versioned.)

CREATE DATABASE regress_versioned_evtrig;
\c regress_versioned_evtrig
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE public.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'before the function')::text) AS committed;
SELECT dolt_branch('evt_old');			-- head-only: no function there

CREATE FUNCTION public.note_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'main''s note_ddl: %', tg_tag;
END $$;
CREATE FUNCTION public.note_drop() RETURNS event_trigger LANGUAGE plpgsql AS $$
DECLARE
	o record;
BEGIN
	FOR o IN SELECT object_type, object_identity FROM pg_event_trigger_dropped_objects()
			  WHERE object_type = 'table' LOOP
		RAISE NOTICE 'dropped % %', o.object_type, o.object_identity;
	END LOOP;
END $$;
CREATE FUNCTION public.note_rewrite() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'rewriting %', pg_event_trigger_table_rewrite_oid()::regclass;
END $$;
CREATE EVENT TRIGGER evt_ddl ON ddl_command_end WHEN TAG IN ('CREATE TABLE')
  EXECUTE FUNCTION public.note_ddl();
CREATE EVENT TRIGGER evt_drop ON sql_drop EXECUTE FUNCTION public.note_drop();
CREATE EVENT TRIGGER evt_rewrite ON table_rewrite EXECUTE FUNCTION public.note_rewrite();
SELECT count(*) AS deps FROM pg_depend
 WHERE classid = 'pg_event_trigger'::regclass AND refclassid = 'pg_proc'::regclass;

CREATE TABLE public.a (id int PRIMARY KEY, v int);
ALTER TABLE public.a ALTER COLUMN v TYPE bigint;
SELECT length(dolt_commit('-Am', 'with the functions')::text) AS committed;
SELECT dolt_hashof('HEAD') AS with_functions \gset

-- a branch with its own version of the function
SELECT dolt_checkout('-b', 'evt_side');
CREATE TABLE public.b (id int PRIMARY KEY);
CREATE OR REPLACE FUNCTION public.note_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'evt_side''s note_ddl: %', tg_tag;
END $$;
CREATE TABLE public.c (id int PRIMARY KEY);
DROP TABLE public.c;
DROP EVENT TRIGGER evt_drop;		-- (its case is made)
SELECT dolt_checkout('main');
CREATE TABLE public.d (id int PRIMARY KEY);

-- a branch dropping its own: skipped there, with a warning; main unchanged
SELECT dolt_checkout('evt_side');
DROP FUNCTION public.note_ddl();
CREATE TABLE public.e (id int PRIMARY KEY);
-- a move making it again, with another OID: found by its name
SELECT dolt_reset('--hard', :'with_functions');
CREATE TABLE public.f (id int PRIMARY KEY);

-- a branch from before the function existed: skipped, and so it is when
-- someone else makes a function of its name there (never run as the
-- trigger's)
CREATE ROLE regress_evt_alice;
GRANT CREATE, USAGE ON SCHEMA public TO regress_evt_alice;
SELECT dolt_checkout('evt_old');
CREATE TABLE public.g (id int PRIMARY KEY);
SET ROLE regress_evt_alice;
CREATE FUNCTION public.note_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'alice''s note_ddl, as superuser: %', current_setting('is_superuser');
END $$;
RESET ROLE;
CREATE TABLE public.g2 (id int PRIMARY KEY);
DROP FUNCTION public.note_ddl();
SELECT dolt_checkout('main');
CREATE TABLE public.h (id int PRIMARY KEY);

-- a trigger on a function in an ordinary schema depends on it, as stock
CREATE SCHEMA plain;
CREATE FUNCTION plain.note() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'plain note: %', tg_tag;
END $$;
CREATE EVENT TRIGGER evt_plain ON ddl_command_end WHEN TAG IN ('CREATE INDEX')
  EXECUTE FUNCTION plain.note();
DROP FUNCTION plain.note();
CREATE INDEX ON public.h (id);

DROP EVENT TRIGGER evt_ddl, evt_rewrite, evt_plain;
\c contrib_regression
DROP DATABASE regress_versioned_evtrig;
DROP ROLE regress_evt_alice;

-- the function's OID gone from every branch (a move made it again): found
-- by the name the trigger noted (which \dy and pg_dump print)
CREATE DATABASE regress_versioned_evtrig2;
\c regress_versioned_evtrig2
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE FUNCTION public.note_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	RAISE NOTICE 'note_ddl: %', tg_tag;
END $$;
CREATE EVENT TRIGGER evt_ddl ON ddl_command_end WHEN TAG IN ('CREATE TABLE')
  EXECUTE FUNCTION public.note_ddl();
SELECT length(dolt_commit('-Am', 'the function')::text) AS committed;
SELECT dolt_hashof('HEAD') AS with_function \gset
DROP FUNCTION public.note_ddl();
SELECT dolt_reset('--hard', :'with_function');
SELECT count(*) AS original_oid_left FROM pg_proc p JOIN pg_event_trigger e ON e.evtfoid = p.oid;
CREATE TABLE public.t (id int PRIMARY KEY);
SELECT evtname, evtfuncname FROM pg_event_trigger;
DROP EVENT TRIGGER evt_ddl;
\c contrib_regression
DROP DATABASE regress_versioned_evtrig2;
