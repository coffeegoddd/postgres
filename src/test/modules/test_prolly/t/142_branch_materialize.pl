# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch made is only its head, as Dolt's is: no catalog rows, no pages, however many are made.  It's
# given a working set as a session checks it out, or on request, at its head, from a branch that has
# one, sharing its pages; two sessions giving it one at once make one.  It gives the working set back
# on request, its head kept, and is given one again row for row; that's refused while another session
# is on it, while it's a database's or role's default_branch, or while it has changes not committed.
# A checkout gives one, for any role, as a session starts too, but not in a transaction block.  Crashes
# leave it whole or a head.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_materialize');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}
sub rows { return sql($_[0], q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}); }
# (every branch's, as no catalog filter hides them)
my $schemas = q{SET catalog_branch_filter = off; SELECT count(*) FROM pg_namespace WHERE nspbranch <> 0};

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql($main, q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; GRANT USAGE ON SCHEMA app TO PUBLIC;
	SET ROLE app_owner; CREATE TABLE app.t (id integer PRIMARY KEY, v text); GRANT SELECT ON app.t TO PUBLIC;
	INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 5) g});
sql($main, "SELECT test_branch_link('main', $main)");
sql($main, q{SELECT test_branch_commit('main', 'c1')});
my $c1 = $node->safe_psql('postgres', q{SELECT test_branch_head('main')});
my $c1_rows = rows($main);
sql($main, q{SET ROLE app_owner; UPDATE app.t SET v = 'c2' WHERE id <= 2});
sql($main, q{SELECT test_branch_commit('main', 'c2')});
my $c2_rows = rows($main);

# Made, they're heads alone.
my $before = $node->safe_psql('postgres', $schemas);
$node->safe_psql('postgres', q{SELECT test_branch_make('f', 'main')});
$node->safe_psql('postgres', qq{SELECT test_branch_make('old', 'main', '$c1')});
$node->safe_psql('postgres', q{SELECT test_branch_make('h' || g, 'main') FROM generate_series(1, 1000) g});
is($node->safe_psql('postgres', $schemas), $before, 'branches made, a thousand and more, have no catalog rows');
is($node->safe_psql('postgres', q{SELECT test_branch_of('f') IS NULL AND test_branch_head('f') IS NOT NULL}), 't',
	'each is only its head');

# Given a working set as a session checks it out, at its head.
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT test_branch_checkout('f')});
my $f = $node->safe_psql('postgres', q{SELECT test_branch_of('f')});
ok($f > 0, 'checked out, a branch is given a working set');
is($s->query_safe(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}), $c2_rows, 'at its head');
is($node->safe_psql('postgres', $schemas), $before + 1, 'and only it');

# Not in a transaction block, as what it's made in is left out of capture.
my ($rc, $out, $err) = $node->psql('postgres', q{BEGIN; SELECT test_branch_checkout('h3'); COMMIT});
like($err, qr/branch "h3" has no working set, and can't be given one in a transaction block/,
	'a branch with no working set isn\'t given one by a checkout in a transaction block');

# By any role, and as a session starts on it.
$node->safe_psql('postgres', q{CREATE ROLE plain LOGIN});
is($node->safe_psql('postgres', q{SET ROLE plain; DO $$BEGIN PERFORM test_branch_checkout('h4'); END$$;
	SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}), $c2_rows,
	'a role with no powers checks out a branch with no working set, which is given one');
{
	local $ENV{PGOPTIONS} = '-c default_branch=h5';
	is($node->safe_psql('postgres', q{SELECT test_branch_current()}, extra_params => ['-U', 'plain']), 'h5',
		'and a session starting on one gives it one');
}
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_create('taker', 'h6', NULL, true)});
like($err, qr/branch "h6" has no working set to take changes from/, 'changes aren\'t taken from a branch with none');

# Given one on request, at an older commit, from a branch at another.
my $old = $node->safe_psql('postgres', q{SELECT test_branch_materialize('old')});
is(rows($old), $c1_rows, 'given one on request, a branch at an older commit has its rows');
is($node->safe_psql('postgres', q{SELECT test_branch_materialize('old')}), $old, 'and given one again, it\'s the same');

# Two at once make one.
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
$a->query_safe(q{BEGIN; SELECT test_branch_materialize('h1')});
$b->query_until(qr/started/, "\\echo started\nSELECT test_branch_materialize('h1');\n\\echo done\n");
my $apid = $a->query_safe('SELECT pg_backend_pid()');
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 1 FROM pg_stat_activity
	WHERE query LIKE 'SELECT test_branch_materialize%' AND $apid = ANY (pg_blocking_pids(pid))}),
	'a second session giving a branch a working set waits for the first');
$a->query_safe('COMMIT');
$out = $b->query_until(qr/done/, '');
my ($h1) = $out =~ /^(\d+)$/m;
is($h1, $node->safe_psql('postgres', q{SELECT test_branch_of('h1')}), 'and has the one it made');
$_->quit for ($a, $b);

# Given back: refused on another session's branch, a default_branch, or with changes not committed.
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_dematerialize('f')});
like($err, qr/branch "f" is checked out/, 'giving back a working set another session is on is refused');
$s->query_safe(q{SELECT test_branch_checkout('main')});
$s->quit;
$node->safe_psql('postgres', q{ALTER DATABASE postgres SET default_branch = 'f'});
{
	# (from a session that names none for itself)
	local $ENV{PGOPTIONS} = '-c default_branch=';
	($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_dematerialize('f')});
}
like($err, qr/branch "f" is a default_branch/, 'and a default_branch\'s');
$node->safe_psql('postgres', q{ALTER DATABASE postgres RESET default_branch; ALTER ROLE plain SET default_branch = 'f'});
{
	local $ENV{PGOPTIONS} = '-c default_branch=';
	($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_dematerialize('f')});
}
like($err, qr/branch "f" is a default_branch/, 'and a role\'s');
$node->safe_psql('postgres', q{ALTER ROLE plain RESET default_branch});
sql($f, q{SET ROLE app_owner; INSERT INTO app.t VALUES (6, 'uncommitted')});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_dematerialize('f')});
like($err, qr/branch "f" has changes not committed/, 'and one with changes not committed');
sql($f, q{SET ROLE app_owner; DELETE FROM app.t WHERE id = 6});

# Given back, its head kept; and given one again, row for row.
$node->safe_psql('postgres', q{SELECT test_branch_dematerialize('f')});
is($node->safe_psql('postgres', q{SELECT test_branch_of('f') IS NULL AND test_branch_head('f') IS NOT NULL}), 't',
	'given back, a branch is only its head again');
is($node->safe_psql('postgres', $schemas), $before + 4, 'its catalog rows gone, the others\' kept');
my $f2 = $node->safe_psql('postgres', q{SELECT test_branch_materialize('f')});
is(rows($f2), $c2_rows, 'given one again, it has its head\'s rows');

# Crashes: given one, it outlasts it; a head, it's a head.
$node->stop('immediate');
$node->start;
is(rows($f2), $c2_rows, 'after a crash, a branch given a working set has it');
is($node->safe_psql('postgres', q{SELECT test_branch_of('h2') IS NULL AND test_branch_head('h2') IS NOT NULL}), 't',
	'and one that was a head is one');

done_testing();
