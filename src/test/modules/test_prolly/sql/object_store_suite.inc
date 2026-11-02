-- The object store conformance suite, run against the store :store at
-- :location (object_store_<store>.sql).
-- (an error's code and message, or none: what is refused, shown the same
-- in every store's output)
CREATE FUNCTION pg_temp.refused(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'not refused';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END$$;
CREATE FUNCTION pg_temp.put_refused(name text, data bytea) RETURNS text LANGUAGE sql
	AS $$SELECT pg_temp.refused(format('SELECT test_os_put(%L, %L, %L, %L)',
		current_setting('test.os_store'), current_setting('test.os_location'), name, data))$$;
SELECT set_config('test.os_store', :'store', false) IS NOT NULL AS store_set,
	set_config('test.os_location', :'location', false) IS NOT NULL AS location_set;
-- (the location emptied first, of what a run cut short may have left)
SELECT count(test_os_remove(:'store', :'location', name)) >= 0 AS emptied
FROM test_os_list(:'store', :'location', '');
-- every implementation is run by a file of its own
SELECT count(*) FILTER (WHERE s = :'store') AS registered FROM test_os_stores() s;

-- an object written whole, read whole and in ranges
SELECT test_os_put(:'store', :'location', 'a/one', '\x0102030405'::bytea);
SELECT test_os_read(:'store', :'location', 'a/one', 0, 5) AS whole,
	test_os_read(:'store', :'location', 'a/one', 1, 3) AS middle,
	test_os_read(:'store', :'location', 'a/one', 5, 0) AS none_at_end;
SELECT test_os_size(:'store', :'location', 'a/one') AS size;
-- an empty one
SELECT test_os_put(:'store', :'location', 'a/empty', ''::bytea);
SELECT test_os_size(:'store', :'location', 'a/empty') AS empty_size;
-- a large one, read in ranges across it
SELECT test_os_put(:'store', :'location', 'b/large',
	(SELECT string_agg(decode(md5(i::text), 'hex'), '' ORDER BY i) FROM generate_series(1, 100000) i));
SELECT test_os_size(:'store', :'location', 'b/large') AS large_size;
SELECT test_os_read(:'store', :'location', 'b/large', 16 * 54321, 16) = decode(md5('54322'), 'hex') AS large_range;

-- never replaced: a second write of the name is an error, the first kept
SELECT pg_temp.put_refused('a/one', '\xff') AS second_write;
SELECT test_os_read(:'store', :'location', 'a/one', 0, 5) AS still_first;

-- not there, or a range not all in it: errors; a size of none is NULL
SELECT pg_temp.refused(format('SELECT test_os_read(%L, %L, %L, 0, 1)', :'store', :'location', 'a/none')) AS none_read;
SELECT pg_temp.refused(format('SELECT test_os_read(%L, %L, %L, 3, 3)', :'store', :'location', 'a/one')) AS short_read;
SELECT pg_temp.refused(format('SELECT test_os_read(%L, %L, %L, 6, 0)', :'store', :'location', 'a/one')) AS past_end;
SELECT test_os_size(:'store', :'location', 'a/none') IS NULL AS no_size;

-- listed in the names' order, by a prefix
SELECT test_os_put(:'store', :'location', 'a/two', '\x02'::bytea);
SELECT name, size FROM test_os_list(:'store', :'location', '');
SELECT name FROM test_os_list(:'store', :'location', 'a/t');

-- removed; removed again, nothing (as object storage's deletes)
SELECT test_os_remove(:'store', :'location', 'a/two') IS NOT NULL AS removed,
	test_os_remove(:'store', :'location', 'a/two') IS NOT NULL AS again;
SELECT name FROM test_os_list(:'store', :'location', 'a/');
-- (a name removed may be written again)
SELECT test_os_put(:'store', :'location', 'a/two', '\x22'::bytea);
SELECT test_os_read(:'store', :'location', 'a/two', 0, 1) AS rewritten;

-- a name and names under it ("a" and "a/one") both objects; one removed,
-- the others there
SELECT test_os_put(:'store', :'location', 'a', '\x0a'::bytea);
SELECT name, size FROM test_os_list(:'store', :'location', 'a');
SELECT test_os_remove(:'store', :'location', 'a') IS NOT NULL AS removed_a;
SELECT test_os_size(:'store', :'location', 'a') IS NULL AS a_gone,
	test_os_size(:'store', :'location', 'a/one') AS one_kept;
-- listed in byte order: "a-" before "a/", "a/" before "a0"
SELECT test_os_put(:'store', :'location', 'a-x', '\x01'::bytea),
	test_os_put(:'store', :'location', 'a0', '\x01'::bytea);
SELECT string_agg(name, ' ' ORDER BY ord) AS listed
FROM test_os_list(:'store', :'location', 'a') WITH ORDINALITY l(name, size, ord);

-- names: lowercase letters, digits and "._-", in parts by "/", none empty,
-- "." or "..", none ending in "."; not beginning with "."; not, to the
-- first ".", a Windows device's; at most 200 bytes
SELECT pg_temp.put_refused('', '\x00') AS refused;
SELECT pg_temp.put_refused('/abs', '\x00') AS refused;
SELECT pg_temp.put_refused('a//b', '\x00') AS refused;
SELECT pg_temp.put_refused('a/../b', '\x00') AS refused;
SELECT pg_temp.put_refused('a/./b', '\x00') AS refused;
SELECT pg_temp.put_refused('a/b/', '\x00') AS refused;
SELECT pg_temp.put_refused('a b', '\x00') AS refused;
SELECT pg_temp.put_refused('Upper', '\x00') AS refused;
SELECT pg_temp.put_refused('a/b.', '\x00') AS refused;
SELECT pg_temp.put_refused('.hidden', '\x00') AS refused;
SELECT pg_temp.put_refused('a#b', '\x00') AS refused;
SELECT pg_temp.put_refused('~tmp', '\x00') AS refused;
SELECT pg_temp.put_refused('nul', '\x00') AS refused;
SELECT pg_temp.put_refused('com1.txt', '\x00') AS refused;
SELECT pg_temp.put_refused('aux/x', '\x00') AS refused;
SELECT pg_temp.put_refused(repeat('n', 201), '\x00') AS refused;
SELECT test_os_put(:'store', :'location', repeat('n', 200), '\x0c'::bytea);
SELECT test_os_size(:'store', :'location', repeat('n', 200)) AS longest;
-- (and these are names)
SELECT test_os_put(:'store', :'location', 'console', '\x01'::bytea),
	test_os_put(:'store', :'location', 'com10', '\x01'::bytea),
	test_os_put(:'store', :'location', 'x.y/.z', '\x01'::bytea);
SELECT test_os_put(:'store', :'location', 'a-z_0.9/ok', '\x09'::bytea);
SELECT test_os_size(:'store', :'location', 'a-z_0.9/ok') AS ok_name;

-- cleaned up
SELECT count(test_os_remove(:'store', :'location', name)) AS removed_all
FROM test_os_list(:'store', :'location', '');
SELECT count(*) AS left FROM test_os_list(:'store', :'location', '');
