-- History operations reproduce state and must not fire users' DDL events.
CREATE DATABASE regress_move_events;
\c regress_move_events
CREATE EXTENSION test_prolly;
CREATE SCHEMA mv VERSIONED;
CREATE TABLE mv.base (id int PRIMARY KEY);
INSERT INTO mv.base VALUES (1);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_hashof('HEAD') AS base \gset
SELECT dolt_branch('old'), dolt_branch('merge_target'), dolt_branch('pick_target');
CREATE TABLE mv.added (id int PRIMARY KEY);
INSERT INTO mv.added VALUES (2);
SELECT length(dolt_commit('-Am', 'added')::text) AS committed;
SELECT dolt_hashof('HEAD') AS added \gset

CREATE TABLE public.events (event text, tag text);
CREATE FUNCTION public.audit_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
  IF current_setting('test.guard_ddl', true) = 'on' THEN
    RAISE EXCEPTION 'user DDL guard: % %', tg_event, tg_tag;
  END IF;
  INSERT INTO public.events VALUES (tg_event, tg_tag);
END $$;
CREATE EVENT TRIGGER mv_start ON ddl_command_start EXECUTE FUNCTION public.audit_ddl();
CREATE EVENT TRIGGER mv_end ON ddl_command_end EXECUTE FUNCTION public.audit_ddl();
CREATE EVENT TRIGGER mv_drop ON sql_drop EXECUTE FUNCTION public.audit_ddl();
CREATE EVENT TRIGGER mv_rewrite ON table_rewrite EXECUTE FUNCTION public.audit_ddl();
SET test.guard_ddl = on;
-- First use creates branch copies and removes objects absent from its head.
SELECT dolt_checkout('old');
SELECT to_regclass('mv.added') IS NULL AS absent;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard', :'base');
SELECT to_regclass('mv.added') IS NULL AS absent;
SELECT dolt_reset('--hard', :'added');
SELECT * FROM mv.added;
SELECT dolt_checkout('merge_target');
INSERT INTO mv.base VALUES (3);
SELECT length(dolt_commit('-Am', 'diverge')::text) AS committed;
SELECT (dolt_merge('main'))[2:4];
SELECT * FROM mv.added;
SELECT dolt_checkout('pick_target');
SELECT (dolt_cherry_pick(:'added'))[2:4];
SELECT * FROM mv.added;
SELECT count(*) AS internal_events FROM public.events;
-- A successful move must not leave user event triggers disabled.
CREATE TABLE mv.guarded (id int);
SET test.guard_ddl = off;
CREATE TABLE mv.ordinary (id int);
ALTER TABLE mv.ordinary ALTER COLUMN id TYPE text;
DROP TABLE mv.ordinary;
SELECT event, tag FROM public.events ORDER BY event, tag;
TRUNCATE public.events;

-- An error inside a move, caught by a subtransaction, also restores triggers.
SELECT set_config('test.base', :'base', false) IS NOT NULL AS saved;
SELECT set_config('test.bad',
  test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '0'), 'obsolete'),
  false) IS NOT NULL AS saved;
SET test.guard_ddl = on;
DO $$ BEGIN
  PERFORM dolt_reset('--hard', current_setting('test.bad'));
EXCEPTION WHEN feature_not_supported THEN
  RAISE NOTICE 'caught obsolete-format reset';
END $$;
CREATE TABLE mv.still_guarded (id int);
SET test.guard_ddl = off;
TRUNCATE public.events;

-- A move invoked inside an event trigger must not append internal commands
-- to the surrounding event's command list, or lose that outer list.
CREATE FUNCTION public.move_inside_event() RETURNS event_trigger LANGUAGE plpgsql AS $$
DECLARE before_count int; after_count int;
BEGIN
  SELECT count(*) INTO before_count FROM pg_event_trigger_ddl_commands();
  PERFORM dolt_reset('--hard', current_setting('test.base'));
  SELECT count(*) INTO after_count FROM pg_event_trigger_ddl_commands();
  RAISE NOTICE 'outer commands before %, after %', before_count, after_count;
END $$;
CREATE EVENT TRIGGER mv_nested ON ddl_command_end WHEN TAG IN ('CREATE SCHEMA')
  EXECUTE FUNCTION public.move_inside_event();
TRUNCATE public.events;
CREATE SCHEMA outer_ddl;
SELECT event, tag FROM public.events ORDER BY event, tag;
SELECT to_regclass('mv.added') IS NULL AS nested_reset;
DROP EVENT TRIGGER mv_nested;
DROP SCHEMA outer_ddl;

-- Clone with a guarding event trigger already installed in the destination.
SELECT dolt_checkout('main');
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/move-events-' || txid_current() AS url \gset
SELECT dolt_remote('add', 'events', :'url');
SELECT dolt_push('events', 'main');
CREATE DATABASE regress_move_events_clone;
\c regress_move_events_clone
CREATE FUNCTION public.guard_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'clone fired % %', tg_event, tg_tag; END $$;
CREATE EVENT TRIGGER clone_start ON ddl_command_start EXECUTE FUNCTION public.guard_ddl();
CREATE EVENT TRIGGER clone_end ON ddl_command_end EXECUTE FUNCTION public.guard_ddl();
CREATE EVENT TRIGGER clone_drop ON sql_drop EXECUTE FUNCTION public.guard_ddl();
SELECT dolt_clone(:'url');
SELECT * FROM mv.added;
CREATE TABLE mv.clone_guarded (id int);
DROP EVENT TRIGGER clone_start, clone_end, clone_drop;
\c regress_move_events
DROP EVENT TRIGGER mv_start, mv_end, mv_drop, mv_rewrite;
\c contrib_regression
DROP DATABASE regress_move_events_clone;
DROP DATABASE regress_move_events;
