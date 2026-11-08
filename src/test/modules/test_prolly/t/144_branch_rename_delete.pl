# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch renamed keeps its head, its working set, changes and all, and the sessions on it: its name
# only.  A branch deleted gives back its working set, changes and all, and its head goes.  As Dolt's:
# each refused for a branch another session is on, unless forced, and a rename onto a branch there
# already, unless forced, which deletes that one; a delete refused for a branch whose head isn't in the
# history of the one this session is on, unless forced, and always for that one itself.  Neither is
# done to "main", or to a branch a database's or role's default_branch names.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_rename_delete');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $main = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $main; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";
sub sql { return $node->safe_psql('postgres', $on . $_[0]); }
sub fails
{
	my ($sql) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', $on . $sql);
	return $err;
}
sub last_line { return (split /\n/, $_[0])[-1]; }
sub rows { return q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM app.t}; }
my $schemas = q{SET catalog_branch_filter = off; SELECT count(*) FROM pg_namespace WHERE nspbranch <> 0};

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text); INSERT INTO app.t SELECT g, 'c1' FROM generate_series(1, 3) g});
sql("SELECT test_branch_link('main', $main)");
sql(q{SELECT test_branch_commit('main', 'c1')});
my $c1 = sql(q{SELECT test_branch_head('main')});
my $base = $node->safe_psql('postgres', $schemas);

# Renamed, a head alone is its head under the new name.
sql(q{SELECT test_branch_make('a', 'main')});
sql(q{SELECT test_branch_rename('a', 'a2')});
is(sql(q{SELECT test_branch_head('a') IS NULL AND test_branch_head('a2') = test_branch_head('main')}), 't',
	'a head alone renamed is its head under the new name');

# One with a working set and a session on it: refused, unless forced; the session stays on it.
sql(q{SELECT test_branch_make('f', 'main')});
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT test_branch_checkout('f')});
$s->query_safe(q{SET ROLE app_owner; UPDATE app.t SET v = 'f' WHERE id = 1; RESET ROLE});
my $f = sql(q{SELECT test_branch_of('f')});
like(fails(q{SELECT test_branch_rename('f', 'f2')}), qr/branch "f" is checked out in another session/,
	'a branch another session is on isn\'t renamed');
sql(q{SELECT test_branch_rename('f', 'f2', true)});
is(sql(q{SELECT test_branch_of('f2')}), $f, 'forced, it is, its working set kept');
is(sql(q{SELECT test_branch_of('f') IS NULL AND test_branch_head('f') IS NULL}), 't', 'and nothing has the old name');
is($s->query_safe(q{SELECT test_branch_current()}), 'f2', 'the session on it is on it by its new name');
is($s->query_safe(rows()), '1:f,2:c1,3:c1', 'its changes not committed with it');
sql(q{SELECT test_branch_commit('f2', 'f')});
isnt(sql(q{SELECT test_branch_head('f2')}), $c1, 'and it commits by the new name');

# This session's own: renamed, it's on it still.
sql(q{SELECT test_branch_make('g', 'main')});
is(last_line($node->safe_psql('postgres', q{SELECT test_branch_checkout('g'); SELECT test_branch_rename('g', 'g2');
	SELECT test_branch_current()})), 'g2', 'the branch this session is on is renamed, and it\'s on it still');

# This session's own, another session on it too: refused, unless forced, as any.
sql(q{SELECT test_branch_make('m', 'main')});
my $s3 = $node->background_psql('postgres');
$s3->query_safe(q{SELECT test_branch_checkout('m')});
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_checkout('m'); SELECT test_branch_rename('m', 'm2')});
like($err, qr/branch "m" is checked out in another session/,
	'the branch this session is on isn\'t renamed while another session is on it too');
$s3->quit;

# A commit waiting on a rename, of the name it read: refused, not made on that name anew.
sql(q{SELECT test_branch_make('r', 'main'); SELECT test_branch_materialize('r')});
my $r_head = sql(q{SELECT test_branch_head('r')});
my $s4 = $node->background_psql('postgres');
$s4->query_safe(q{BEGIN; SELECT test_branch_rename('r', 'r2')});
my $s5 = $node->background_psql('postgres', on_error_stop => 0);
$s5->query_until(qr/started/, "\\echo started\nSELECT test_branch_commit('r', 'late');\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}) or die 'no wait';
$s4->query_safe(q{COMMIT});
$s4->quit;
$s5->query(q{SELECT 1});
like($s5->{stderr}, qr/branch "r" changed as it was being committed/, 'a commit waiting on a rename is refused');
$s5->quit;
is(sql(q{SELECT test_branch_head('r') IS NULL}), 't', 'a commit waiting on a rename makes no branch of the old name');
is(sql(q{SELECT test_branch_head('r2')}), $r_head, 'nor commits on the new one');

# Onto a branch there already: refused, unless forced, which deletes it, its working set too.
sql(q{SELECT test_branch_make('x', 'main'); SELECT test_branch_make('y', 'main'); SELECT test_branch_materialize('y')});
my $with_y = $node->safe_psql('postgres', $schemas);
like(fails(q{SELECT test_branch_rename('x', 'y')}), qr/a branch named "y" already exists/,
	'a branch isn\'t renamed onto one there already');
sql(q{SELECT test_branch_rename('x', 'y', true)});
is(sql(q{SELECT test_branch_of('y') IS NULL AND test_branch_head('x') IS NULL AND test_branch_head('y') IS NOT NULL}), 't',
	'forced, it is, the one there deleted');
is($node->safe_psql('postgres', $schemas), $with_y - 1, 'its working set given back');

# This session's branch, left for another in the delete's transaction: deleted.
sql(q{SELECT test_branch_make('b1', 'main'); SELECT test_branch_materialize('b1')});
$node->safe_psql('postgres', qq{SELECT test_branch_checkout('b1');
	BEGIN; SET catalog_branch = $main; SELECT test_branch_delete('b1'); COMMIT});
is(sql(q{SELECT test_branch_head('b1') IS NULL}), 't',
	'a branch this session left in the delete\'s own transaction is deleted');

# Neither "main" nor a default_branch, nor to the same name.
like(fails(q{SELECT test_branch_rename('main', 'trunk', true)}), qr/branch "main" can't be renamed/, '"main" isn\'t renamed');
like(fails(q{SELECT test_branch_rename('a2', 'main', true)}), qr/branch "main" can't be replaced/, 'nor replaced');
like(fails(q{SELECT test_branch_rename('a2', 'a2')}), qr/branch "a2" is already named so/, 'nor one to its own name');
$node->safe_psql('postgres', q{ALTER DATABASE postgres SET default_branch = 'a2'});
like(fails(q{SELECT test_branch_rename('a2', 'a3', true)}), qr/branch "a2" is a default_branch/,
	'nor a default_branch, even forced');
like(fails(q{SELECT test_branch_delete('a2', true)}), qr/branch "a2" is a default_branch/,
	'which isn\'t deleted either, even forced');
$node->safe_psql('postgres', q{ALTER DATABASE postgres RESET default_branch});

# Deleted: a head alone, merged.
sql(q{SELECT test_branch_delete('a2')});
is(sql(q{SELECT test_branch_head('a2') IS NULL}), 't', 'a head alone, in the history of this one\'s, is deleted');
like(fails(q{SELECT test_branch_delete('a2')}), qr/branch "a2" not found/, 'and isn\'t there to delete again');

# One with commits of its own: refused, unless forced; another session on it too.
like(fails(q{SELECT test_branch_delete('f2')}), qr/branch "f2" is checked out in another session/,
	'a branch another session is on isn\'t deleted');
$s->query_safe(q{SELECT test_branch_checkout('main')});
like(fails(q{SELECT test_branch_delete('f2')}), qr/branch "f2" is not fully merged/,
	'nor one with commits this one\'s history hasn\'t');
my $with_f2 = $node->safe_psql('postgres', $schemas);
sql(q{SELECT test_branch_delete('f2', true)});
is(sql(q{SELECT test_branch_of('f2') IS NULL AND test_branch_head('f2') IS NULL}), 't', 'forced, it is');
is($node->safe_psql('postgres', $schemas), $with_f2 - 1, 'its working set given back');

# A commit waiting on a delete's check, of a commit not merged: the delete refused.
sql(q{SELECT test_branch_make('k2', 'main'); SELECT test_branch_materialize('k2')});
my $s6 = $node->background_psql('postgres');
$s6->query_safe(q{BEGIN; SELECT test_branch_commit('k2', 'unmerged')});
my $s7 = $node->background_psql('postgres', on_error_stop => 0);
$s7->query_until(qr/started/, $on . "\\echo started\nSELECT test_branch_delete('k2');\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}) or die 'no wait';
$s6->query_safe(q{COMMIT});
$s6->quit;
$s7->query(q{SELECT 1});
like($s7->{stderr}, qr/branch "k2" is not fully merged/, 'a delete waiting on a commit not merged is refused');
$s7->quit;
is(sql(q{SELECT test_branch_head('k2') IS NOT NULL}), 't',
	'a delete waiting on a commit not merged is refused, as it checks the head committed');

# Merged into the branch this session is on, not "main".
sql(q{SELECT test_branch_make('p', 'main'); SELECT test_branch_materialize('p'); SELECT test_branch_commit('p', 'p1');
	SELECT test_branch_make('q', 'p')});
like(fails(q{SELECT test_branch_delete('q')}), qr/branch "q" is not fully merged/,
	'one in the history of another branch only isn\'t deleted from "main"');
$node->safe_psql('postgres', q{SELECT test_branch_checkout('p'); SELECT test_branch_delete('q')});
is(sql(q{SELECT test_branch_head('q') IS NULL}), 't', 'but is from that branch');

# Forced, one another session is on.
sql(q{SELECT test_branch_make('k', 'main')});
$s->query_safe(q{SELECT test_branch_checkout('k')});
sql(q{SELECT test_branch_delete('k', true)});
is(sql(q{SELECT test_branch_head('k') IS NULL}), 't', 'forced, a branch another session is on is deleted');
is($s->query_safe(q{SELECT test_branch_current() IS NULL}), 't', 'and that session is on none');
$s->quit;

# One with changes not committed, its head merged: deleted, changes and all.
sql(q{SELECT test_branch_make('d', 'main')});
$node->safe_psql('postgres', q{SELECT test_branch_checkout('d'); SET ROLE app_owner; DELETE FROM app.t});
sql(q{SELECT test_branch_delete('d')});
is(sql(q{SELECT test_branch_head('d') IS NULL AND test_branch_of('d') IS NULL}), 't',
	'a branch with changes not committed is deleted, as its head is merged');

# Never "main", nor the one this session is on.
like(fails(q{SELECT test_branch_delete('main', true)}), qr/branch "main" can't be deleted/, '"main" isn\'t deleted');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_checkout('g2'); SELECT test_branch_delete('g2', true)});
like($err, qr/branch "g2" is checked out in this session/, 'nor the branch this session is on, even forced');
($rc, $out, $err) = $node->psql('postgres', qq{SELECT test_branch_checkout('g2'); DO \$\$BEGIN
	PERFORM set_config('catalog_branch', '$main', true); PERFORM test_branch_delete('g2', true); END\$\$});
like($err, qr/branch "g2" is checked out in this session/, 'nor by what moves off it for itself only');

# A name deleted is made again from scratch.
sql(q{SELECT test_branch_make('d', 'main'); SELECT test_branch_materialize('d')});
is(last_line($node->safe_psql('postgres', q{SELECT test_branch_checkout('d');} . rows())), '1:c1,2:c1,3:c1',
	'a name deleted is made again, at its new head');

# In one statement with a write, as either is left out of capture: refused.
sql(q{SELECT test_branch_make('w', 'main'); SELECT test_branch_materialize('w')});
like(fails(q{DO $$BEGIN PERFORM test_branch_delete('w'); INSERT INTO app.t VALUES (9, 'z'); END$$}),
	qr/cannot write to versioned table "t" in a transaction left out of capture/,
	'a write after a delete, in its statement, is refused');
is(sql(q{SELECT test_branch_of('w') IS NOT NULL}), 't', 'and the delete with it');

done_testing();
