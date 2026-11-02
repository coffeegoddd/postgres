# Two writers of one object's name at once: the one linking its file into
# place first wins, and the other's write is refused, "there already", its
# temporary file gone; the object the winner's whole.  The first stops, by
# an injection point of its session alone, with its bytes written and
# synced, before it links them.

setup
{
	CREATE EXTENSION IF NOT EXISTS test_prolly;
	CREATE EXTENSION IF NOT EXISTS injection_points;
}

teardown
{
	SELECT test_os_remove('directory', 'pg_test_object_race', 'r/x');
}

session a
setup		{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('object-store-written', 'wait');
}
step a_put		{ SELECT test_os_put('directory', 'pg_test_object_race', 'r/x', '\x0a0a'::bytea); }
step a_detach	{ SELECT injection_points_detach('object-store-written'); }

session b
step b_put		{ SELECT test_os_put('directory', 'pg_test_object_race', 'r/x', '\x0b0b0b'::bytea); }
step b_check	{
	SELECT test_os_read('directory', 'pg_test_object_race', 'r/x', 0, 3) AS bytes,
		(SELECT string_agg(name, ',') FROM test_os_list('directory', 'pg_test_object_race', '')) AS listed,
		(SELECT string_agg(f, ',') FROM pg_ls_dir('pg_test_object_race') f) AS files;
}

session o
step o_wake		{ SELECT injection_points_wakeup('object-store-written'); }

permutation a_put b_put o_wake a_detach b_check
