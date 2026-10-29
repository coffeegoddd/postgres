-- Every database gets these, outside the versioned schema.
CREATE EXTENSION amcheck;
CREATE EXTENSION prolly_pages;

-- Event triggers that log every DDL command, one enabled ALWAYS and one
-- REPLICA (the mode version control runs in).  In a database a checkout,
-- merge or rebuild wrote, vc_noise must stay empty: version control runs
-- no user code.
CREATE TABLE public.vc_noise (what text);
CREATE FUNCTION public.vc_noise_log() RETURNS event_trigger LANGUAGE plpgsql AS
	$$BEGIN INSERT INTO public.vc_noise VALUES (tg_tag); END$$;
CREATE EVENT TRIGGER noise_always ON ddl_command_end EXECUTE FUNCTION public.vc_noise_log();
ALTER EVENT TRIGGER noise_always ENABLE ALWAYS;
CREATE EVENT TRIGGER noise_replica ON ddl_command_end EXECUTE FUNCTION public.vc_noise_log();
ALTER EVENT TRIGGER noise_replica ENABLE REPLICA;
