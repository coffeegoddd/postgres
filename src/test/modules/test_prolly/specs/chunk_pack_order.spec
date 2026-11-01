# Two packs naming the same chunks in other orders, at once: their entries
# go in by name, so the second waits for the first's put of the chunk both
# name first, and neither waits for the other while the other waits for
# it (in the packs' own orders, each would have one chunk in, and wait for
# the other's: a deadlock).  The first pack stops, by an injection point
# of its session alone, after its first entry.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
	CREATE OR REPLACE FUNCTION pack_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
}

session b
setup		{
	SET chunk_free_in_background = off;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('chunk-pack-entry', 'wait');
}
step b_begin	{ BEGIN; }
step b_pack		{
	SELECT test_cs_put_many('pages', ARRAY[pack_h('\x0a0301'), pack_h('\x0a0302')],
		ARRAY['\x0a0301', '\x0a0302']::bytea[]) AS stored;
}
step b_commit	{ COMMIT; }
step b_free		{
	SELECT test_cs_free('pages', ARRAY[pack_h('\x0a0301'), pack_h('\x0a0302')]) AS freed;
}

session d
setup		{ SET chunk_free_in_background = off; }
step d_begin	{ BEGIN; }
step d_pack		{
	SELECT test_cs_put_many('pages', ARRAY[pack_h('\x0a0302'), pack_h('\x0a0301')],
		ARRAY['\x0a0302', '\x0a0301']::bytea[]) AS stored;
}
step d_commit	{ COMMIT; }

session c
step c_wake		{
	SELECT injection_points_detach('chunk-pack-entry');
	SELECT injection_points_wakeup('chunk-pack-entry');
}

# b stops after its first entry; d waits for it; b goes on, and d then has
# both
permutation b_begin d_begin b_pack d_pack c_wake b_commit d_commit b_free
