# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's root: each of its versioned schemas' roots, by name, as of one point.  A transaction
# that writes two of them is in it whole or not at all; another branch's schemas aren't in it; a
# branch with none has the empty root.  Committed on its branch of version control, and restored on
# another catalog branch, each schema there has the root it had.  Each branch of the repository is
# linked to one catalog branch, and the other way about.  A schema made on the branch while its root
# is being taken is a serialization failure, not a root without it.

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

my ($b, $other, $empty, $restored) =
  map { $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()') } 1 .. 4;
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
my ($rc, $out, $err) = $node->psql('postgres', "SELECT test_branch_link('main', $other)");
like($err, qr/branch "main" or catalog branch $other is linked already/, 'a name linked twice is refused');
($rc, $out, $err) = $node->psql('postgres', "SELECT test_branch_link('again', $b)");
like($err, qr/branch "again" or catalog branch $b is linked already/, 'and a catalog branch linked twice');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_link('never', 999999)});
like($err, qr/branch 999999 has not been allocated/, 'and one never allocated');
($rc, $out, $err) = $node->psql('postgres', "SELECT test_branch_link('', $other)");
like($err, qr/a branch's name can't be empty/, 'and an empty name');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_commit('nowhere', 'c')});
like($err, qr/branch "nowhere" is linked to no catalog branch/, 'a commit of a branch not linked is refused');

# Committed, and restored on another catalog branch: each schema has the root it had.
my $committed = sql($b, q{SELECT test_branch_commit('main', 'c1')});
is($committed, $both, 'a commit of the branch holds its root');
sql($restored, "SELECT test_dbroot_restore('$committed', $restored)");
is(sql($restored, "SELECT test_branch_barrier($restored)"), $committed, 'restored on another catalog branch, it is the same root');
is(sql($restored, q{SELECT string_agg(id || v, ',' ORDER BY id) FROM app.t}), '1a,2both', 'with its rows');
is(sql($b, q{SELECT string_agg(id || v, ',' ORDER BY id) FROM app.t}), '1a,2both', 'and the branch it came from as it was');

SKIP:
{
	skip 'injection points not supported by this build', 2 unless $injections;

	# A schema made on the branch while its root is taken, after the schemas were read.
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-barrier-locked', 'wait')});
	my $taker = $node->background_psql('postgres', on_error_stop => 0);
	$taker->query_safe(on($b));
	$taker->query_until(qr/started/, "\\echo started\nSELECT test_branch_barrier($b);\n\\echo done\n");
	$node->wait_for_event('client backend', 'capture-barrier-locked');
	sql($b, q{CREATE SCHEMA late VERSIONED});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-barrier-locked'); SELECT injection_points_wakeup('capture-barrier-locked')});
	$taker->query_until(qr/done/, '');
	like($taker->{stderr}, qr/a schema of branch $b was made during a commit from capture/,
		'a schema made while a branch\'s root is taken is a serialization failure');
	$taker->quit;
	like(sql($b, "SELECT test_dbroot_schemas(test_branch_barrier($b))"), qr/^app=\w+,inv=\w+,late=\w+$/,
		'and tried again, the root has it');
}

done_testing();
