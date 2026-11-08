# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's root: each of its versioned schemas' roots, by name, as of one point.  A transaction
# that writes two of them is in it whole or not at all, and one that commits after the marker is in
# neither; another branch's schemas aren't in it; a branch with none has the empty root, and one never
# allocated has none.  It's committed on its branch of version control.  Each branch of the repository
# is linked to one catalog branch, and the other way about.  A schema made, or renamed, on the branch
# while its root is being taken is a serialization failure, not a root without it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_roots');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $injections = $ENV{enable_injection_points} eq 'yes' && !$windows_os;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points') if $injections;

my ($b, $other, $empty) =
  map { $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()') } 1 .. 3;
sub on { return "SET catalog_branch = $_[0]; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n"; }
sub sql
{
	my ($branch, $sql) = @_;
	return $node->safe_psql('postgres', on($branch) . $sql);
}

sql($b, q{CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (id integer PRIMARY KEY, v text);
	INSERT INTO app.t VALUES (1, 'a');
	CREATE SCHEMA inv VERSIONED; CREATE TABLE inv.u (id integer PRIMARY KEY); INSERT INTO inv.u VALUES (1)});
sql($other, q{CREATE SCHEMA app VERSIONED; CREATE TABLE app.t (id integer PRIMARY KEY, v text);
	CREATE SCHEMA elsewhere VERSIONED});

# Each schema's root, by name, as a schema's own barrier has it; another branch's not there.
my $root = sql($b, "SELECT test_branch_barrier($b)");
my $app = sql($b, q{SELECT test_capture_barrier('app')});
my $inv = sql($b, q{SELECT test_capture_barrier('inv')});
is(sql($b, "SELECT test_dbroot_schemas('$root')"), "app=$app,inv=$inv", 'a branch\'s root holds each of its schemas\' roots');
like(sql($other, "SELECT test_dbroot_schemas(test_branch_barrier($other))"), qr/^app=\w+,elsewhere=\w+$/,
	'and another branch\'s, its own');
is(sql($empty, "SELECT test_dbroot_schemas(test_branch_barrier($empty))"), '', 'a branch with no schema has the empty root');
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_barrier(999999)});
like($err, qr/branch 999999 has not been allocated/, 'and one never allocated has none');

# A transaction writing two schemas: in it whole, or not at all.
my $writer = $node->background_psql('postgres');
$writer->query_safe(on($b));
$writer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (2, 'both'); INSERT INTO inv.u VALUES (2)});
is(sql($b, "SELECT test_branch_barrier($b)"), $root, 'a transaction open on both schemas is in neither');
$writer->query_safe('COMMIT');
$writer->quit;
my $both = sql($b, "SELECT test_branch_barrier($b)");
my ($app2, $inv2) = $both ne $root
  ? (sql($b, q{SELECT test_capture_barrier('app')}), sql($b, q{SELECT test_capture_barrier('inv')})) : ('', '');
is(sql($b, "SELECT test_dbroot_schemas('$both')"), "app=$app2,inv=$inv2", 'once it commits, it is in both');
isnt($app2, $app, 'app changed');
isnt($inv2, $inv, 'and inv');

# The link: one name is one catalog branch.
sql($b, "SELECT test_branch_link('main', $b)");
is(sql($b, q{SELECT test_branch_of('main')}), $b, 'a branch linked');
is(sql($b, q{SELECT test_branch_of('nowhere') IS NULL}), 't', 'one not linked has none');
($rc, $out, $err) = $node->psql('postgres', "SELECT test_branch_link('main', $other)");
like($err, qr/branch "main" or catalog branch $other is linked already/, 'a name linked twice is refused');
($rc, $out, $err) = $node->psql('postgres', "SELECT test_branch_link('again', $b)");
like($err, qr/branch "again" or catalog branch $b is linked already/, 'and a catalog branch linked twice');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_link('never', 999999)});
like($err, qr/branch 999999 has not been allocated/, 'and one never allocated');
($rc, $out, $err) = $node->psql('postgres', "SELECT test_branch_link('', $other)");
like($err, qr/a branch's name can't be empty/, 'and an empty name');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_commit('nowhere', 'c')});
like($err, qr/branch "nowhere" is linked to no catalog branch/, 'a commit of a branch not linked is refused');

# Committed; and a link let go of, and made again.
my $committed = sql($b, q{SELECT test_branch_commit('main', 'c1')});
is($committed, $both, 'a commit of the branch holds its root');
sql($other, "SELECT test_branch_link('side', $other)");
is(sql($other, q{SELECT test_branch_unlink('side')}), 't', 'a link let go of');
is(sql($other, q{SELECT test_branch_of('side') IS NULL}), 't', 'is gone');
is(sql($other, q{SELECT test_branch_unlink('side')}), 'f', 'and let go of again, there was none');
sql($other, "SELECT test_branch_link('side2', $other)");
is(sql($other, q{SELECT test_branch_of('side2')}), $other, 'its catalog branch linked again, under another name');

SKIP:
{
	skip 'injection points not supported by this build', 5 unless $injections;

	# A transaction writing both schemas, committed after the marker: in neither, as one point.
	my $before = sql($b, "SELECT test_branch_barrier($b)");
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-barrier-saved', 'wait')});
	my $taker = $node->background_psql('postgres', on_error_stop => 0);
	$taker->query_safe(on($b));
	$taker->query_until(qr/started/, "\\echo started\nSELECT test_branch_barrier($b);\n\\echo done\n");
	$node->wait_for_event('client backend', 'capture-barrier-saved');
	sql($b, q{INSERT INTO app.t VALUES (3, 'after'); INSERT INTO inv.u VALUES (3)});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-barrier-saved'); SELECT injection_points_wakeup('capture-barrier-saved')});
	my $got = $taker->query_until(qr/done/, '');
	$taker->quit;
	like($got, qr/^\Q$before\E$/m, 'a transaction on both schemas committed after the marker is in neither');
	isnt(sql($b, "SELECT test_branch_barrier($b)"), $before, 'and the next root has it');

	# A schema made, then one renamed, on the branch while its root is taken, after the schemas were read.
	for my $case (['made', q{CREATE SCHEMA late VERSIONED}, qr/a schema of branch $b was made during a commit from capture/],
		['renamed', q{ALTER SCHEMA inv RENAME TO inv2}, qr/schema "inv" changed during a commit from capture/])
	{
		my ($what, $ddl, $refused) = @$case;
		$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-barrier-locked', 'wait')});
		$taker = $node->background_psql('postgres', on_error_stop => 0);
		$taker->query_safe(on($b));
		$taker->query_until(qr/started/, "\\echo started\nSELECT test_branch_barrier($b);\n\\echo done\n");
		$node->wait_for_event('client backend', 'capture-barrier-locked');
		sql($b, $ddl);
		$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-barrier-locked'); SELECT injection_points_wakeup('capture-barrier-locked')});
		$taker->query_until(qr/done/, '');
		like($taker->{stderr}, $refused, "a schema $what while a branch's root is taken is a serialization failure");
		$taker->quit;
	}
	like(sql($b, "SELECT test_dbroot_schemas(test_branch_barrier($b))"), qr/^app=\w+,inv2=\w+,late=\w+$/,
		'and tried again, the root has them as they are');
}

done_testing();
