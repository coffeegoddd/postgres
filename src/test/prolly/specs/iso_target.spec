# The tables the isolation group creates are on the access method it is
# meant to test.  If the group's settings were not applied,
# prolly_test.table_am is unset and this fails, rather than the rest of the
# group quietly testing whatever access method the server defaults to.

setup
{
	CREATE TABLE target_probe (a int);
}

teardown
{
	DROP TABLE target_probe;
}

session probe
step check_target
{
	SELECT am.amname = current_setting('prolly_test.table_am') AS on_target
	FROM pg_class c JOIN pg_am am ON am.oid = c.relam
	WHERE c.oid = 'target_probe'::regclass;
}

permutation check_target
