-- Every database gets these, outside the versioned schema.
CREATE EXTENSION amcheck;
CREATE EXTENSION prolly_pages;

-- The generation function counts its calls here.  Where version control
-- wrote rows (a rebuild, a checkout, a merge's result), the count must
-- stay at zero: what a commit holds is copied, not computed again.
CREATE SEQUENCE public.vc_gen_calls;

-- Event triggers that log every DDL command; vc_noise must stay empty
-- where version control wrote.
CREATE TABLE public.vc_noise (what text);
CREATE FUNCTION public.vc_noise_log() RETURNS event_trigger LANGUAGE plpgsql AS
	$$BEGIN INSERT INTO public.vc_noise VALUES (tg_tag); END$$;
CREATE EVENT TRIGGER noise_always ON ddl_command_end EXECUTE FUNCTION public.vc_noise_log();
ALTER EVENT TRIGGER noise_always ENABLE ALWAYS;
