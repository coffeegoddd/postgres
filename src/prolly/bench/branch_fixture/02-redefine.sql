-- The function a generation expression calls is replaced.  Postgres does
-- not rewrite the table: the rows already there keep the values the old
-- body gave, the new rows get the new body's.  A checkout must keep both;
-- computing them again would give every row the new body's value.
SET search_path = rt, public;
CREATE OR REPLACE FUNCTION scale(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$
BEGIN
	PERFORM nextval('public.vc_gen_calls');
	RETURN $1 * 100;
END $$;
INSERT INTO item (a, b) SELECT g, 'after' || g FROM generate_series(1, 10) g;
INSERT INTO bag VALUES (4);
UPDATE byd SET a = a + 1 WHERE id <= 5;
