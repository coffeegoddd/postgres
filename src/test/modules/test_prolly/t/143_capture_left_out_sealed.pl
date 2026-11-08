# Copyright (c) 2026, PostgreSQL Global Development Group

# A transaction left out of capture, its version control work done (a move in place, here), refuses
# what it would write after to a versioned table, or change of a versioned schema's objects, as that
# would be in no branch's history: an insert, a truncate, a table made, in the same statement as the
# move, or a checkout giving a branch its working set, or a branch giving it back.  A table whose rows
# the move told capture, altered after, is refused at commit.  A shared
# table is written as ever, and the move alone, then writes in a transaction of their own, are as any.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('left_out_sealed');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE TABLE public.plain (id integer)');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";
sub sql { return $node->safe_psql('postgres', $on . $_[0]); }

$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner;
	CREATE TABLE app.t (id integer PRIMARY KEY, v text); INSERT INTO app.t VALUES (1, 'a'), (2, 'a')});
my $first = sql(q{SELECT test_capture_commit_on('app', 'main', 'c1')});
sql(q{SET ROLE app_owner; UPDATE app.t SET v = 'b'});
my $second = sql(q{SELECT test_capture_commit_on('app', 'main', 'c2')});

# (each a move back to the first, then something else, in one statement)
sub after_move
{
	my ($root, $then) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', $on . qq{DO \$\$BEGIN
		PERFORM test_ws_move('app', '$root'); $then; END\$\$});
	return $err;
}

like(after_move($first, q{INSERT INTO app.t VALUES (3, 'c')}),
	qr/cannot write to versioned table "t" in a transaction left out of capture/,
	'an insert after a move, in its transaction, is refused');
like(after_move($first, q{TRUNCATE app.t}), qr/cannot truncate versioned table "t" in a transaction left out of capture/,
	'and a truncate');
like(after_move($first, q{CREATE TABLE app.x (id integer)}),
	qr/cannot change a versioned schema's objects in a transaction left out of capture/, 'and a table made');
like(after_move($first, q{ALTER TABLE app.t ADD COLUMN w integer}),
	qr/table "t" was changed after its rows were told to capture/, 'and a table its rows told of altered, at commit');
is(sql(q{SELECT string_agg(id || v, ',' ORDER BY id) FROM app.t}), '1b,2b', 'each undone whole');

is(after_move($first, q{INSERT INTO public.plain VALUES (1)}), '', 'a shared table is written after a move as ever');
is(sql(q{SELECT string_agg(id || v, ',' ORDER BY id) FROM app.t}), '1a,2a', 'and the move with it');
is(sql(q{SELECT test_capture_barrier('app')}), $first, 'capture has it');
sql(q{SET ROLE app_owner; INSERT INTO app.t VALUES (3, 'c')});
is(sql(q{SELECT test_capture_barrier('app')}), sql(q{SELECT test_ws_root_capture('app')}),
	'and a write in a transaction of its own after, as any');

# A branch given its working set by a checkout, or giving it back, in a statement: the same.
sql("SELECT test_branch_link('main', $branch)");
sql(q{SELECT test_branch_commit('main', 'c3')});
$node->safe_psql('postgres', q{SELECT test_branch_make('h', 'main'); SELECT test_branch_make('g', 'main')});
my ($rc, $out, $err) = $node->psql('postgres', q{DO $$BEGIN PERFORM test_branch_checkout('h');
	INSERT INTO app.t VALUES (9, 'z'); END$$});
like($err, qr/cannot write to versioned table "t" in a transaction left out of capture/,
	'a write after a checkout gives a branch its working set, in its statement, is refused');
is($node->safe_psql('postgres', q{SELECT test_branch_of('h') IS NULL}), 't', 'and the working set with it');
$node->safe_psql('postgres', q{SELECT test_branch_materialize('g')});
($rc, $out, $err) = $node->psql('postgres', $on . q{DO $$BEGIN PERFORM test_branch_dematerialize('g');
	INSERT INTO app.t VALUES (9, 'z'); END$$});
like($err, qr/cannot write to versioned table "t" in a transaction left out of capture/,
	'and one after a branch gives its working set back');

done_testing();
