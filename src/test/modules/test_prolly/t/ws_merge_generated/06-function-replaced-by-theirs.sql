-- Theirs replaces the function the expression calls, and each side changes a source of
-- one row: it is computed by the merged function, whichever side replaced it.
-- ours
UPDATE rt.item SET a = 9 WHERE id = 2;
-- theirs
CREATE OR REPLACE FUNCTION rt.scaled(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$
BEGIN
	PERFORM nextval('public.vc_gen_calls');
	RETURN $1 * 100;
END $$;
UPDATE rt.item SET b = 5 WHERE id = 2;
-- expect
clean
check SELECT total FROM rt.item WHERE id = 2 => 905
