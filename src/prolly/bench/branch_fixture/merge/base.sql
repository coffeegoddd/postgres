-- The base every generated-column merge branches from: a stored generated
-- column over two columns, through a user function that counts its calls.
CREATE SCHEMA rt;
CREATE FUNCTION rt.scale(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$
BEGIN
	PERFORM nextval('public.vc_gen_calls');
	RETURN $1 * 10;
END $$;
CREATE TABLE rt.item (
	id int PRIMARY KEY,
	a int,
	b int,
	total int GENERATED ALWAYS AS (rt.scale(a) + b) STORED
);
CREATE TABLE rt.bag (x int, gx int GENERATED ALWAYS AS (x * 2) STORED);
INSERT INTO rt.item SELECT g, g, 1 FROM generate_series(1, 100) g;
INSERT INTO rt.bag VALUES (1), (2);
