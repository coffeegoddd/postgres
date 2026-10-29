-- Ours replaces the function the expression calls (no rewrite: no row
-- changes); theirs changes a row under the old body.  A row one side wrote
-- under the merged expression is kept as written, as Postgres keeps it:
-- the merge computes nothing for it.
-- ours
CREATE OR REPLACE FUNCTION rt.scale(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$
BEGIN
	PERFORM nextval('public.vc_gen_calls');
	RETURN $1 * 100;
END $$;
-- theirs
UPDATE rt.item SET a = 9 WHERE id = 2;
-- expect
clean
check SELECT total FROM rt.item WHERE id = 2 => 91
