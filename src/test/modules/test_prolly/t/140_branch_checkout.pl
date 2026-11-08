# Copyright (c) 2026, PostgreSQL Global Development Group

# A session checks a branch out by name, as any role may: it's then on that branch's catalog branch,
# seeing its tables, and another session can tell it's in use, and the one it would return to.  A
# checkout rolled back, or rolled back to a savepoint, is undone; of several in one transaction, the
# last is kept; one in a savepoint released stands, and so does one in a function; a function's own
# setting is undone as it returns; after a reset the session is where it started.  What another can
# tell is always where the session is or would return to, until it's gone, mid-transaction or not.  One in a transaction that has changed a versioned schema is refused, and one of a
# branch not there.  While a change has a branch from its sessions, a checkout of it waits.  A new
# session checks out the database's default branch, or the one its connection names.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_checkout');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE TABLE public.plain (id integer)');

my ($main, $feat) = map { $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()') } 1 .. 2;
for my $b ([$main, 'main'], [$feat, 'feat'])
{
	my ($id, $name) = @$b;
	$node->safe_psql('postgres', qq{SET catalog_branch = $id; CREATE SCHEMA app VERSIONED;
		CREATE TABLE app.t (v text); INSERT INTO app.t VALUES ('$name'); GRANT USAGE ON SCHEMA app TO PUBLIC;
		GRANT SELECT ON app.t TO PUBLIC; SELECT test_branch_link('$name', $id)});
}

sub in_use { return $node->safe_psql('postgres', "SELECT test_branch_in_use($_[0])"); }

# Checked out: the session is on it, and holds it.
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT test_branch_checkout('feat')});
is($s->query_safe('SHOW catalog_branch'), $feat, 'a checkout puts the session on the branch');
is($s->query_safe('SELECT v FROM app.t'), 'feat', 'seeing its tables');
is($s->query_safe('SELECT test_branch_current()'), 'feat', 'and it is the branch the session is on');
is(in_use($feat), 't', 'another session sees it in use');
is(in_use($main), 'f', 'and not the branch it isn\'t on');

# Several in one transaction: the last kept, the one before let go.
$s->query_safe(q{BEGIN; SELECT test_branch_checkout('main'); SELECT test_branch_checkout('feat');
	SELECT test_branch_checkout('main'); COMMIT});
is($s->query_safe('SHOW catalog_branch'), $main, 'of several checkouts committed, the last is kept');
is(in_use($main) . in_use($feat), 'tf', 'it alone held');

# Rolled back, and rolled back to a savepoint: undone, the hold given back.
$s->query_safe(q{BEGIN; SELECT test_branch_checkout('feat')});
is(in_use($feat), 't', 'a checkout in a transaction holds the branch');
is(in_use($main), 't', 'and the one it would return to, were it rolled back');
$s->query_safe('ROLLBACK');
is($s->query_safe('SHOW catalog_branch'), $main, 'rolled back, the session is where it was');
is(in_use($main) . in_use($feat), 'tf', 'holding what it held');
$s->query_safe(q{BEGIN; SAVEPOINT a; SELECT test_branch_checkout('feat'); ROLLBACK TO a; COMMIT});
is($s->query_safe('SHOW catalog_branch'), $main, 'rolled back to a savepoint, it is undone');
is(in_use($main) . in_use($feat), 'tf', 'its hold too');

# Released, then a sibling rolled back: the checkout stands, and is held.
$s->query_safe(q{BEGIN; SAVEPOINT a; SELECT test_branch_checkout('feat'); RELEASE a; SAVEPOINT b; ROLLBACK TO b; COMMIT});
is($s->query_safe('SHOW catalog_branch'), $feat, 'a checkout in a savepoint released stands, a sibling rolled back');
is(in_use($main) . in_use($feat), 'ft', 'and is held');
$s->query_safe(q{SELECT test_branch_checkout('main')});

# In a function of a versioned schema: it stands as the function returns, as the setting does.
$node->safe_psql('postgres', qq{SET catalog_branch = $main; CREATE FUNCTION app.go() RETURNS void LANGUAGE plpgsql
	AS \$\$BEGIN PERFORM test_branch_checkout('feat'); END\$\$});
$s->query_safe(q{SELECT app.go()});
is($s->query_safe('SHOW catalog_branch'), $feat, 'a checkout in a function stands as it returns');
is(in_use($main) . in_use($feat), 'ft', 'and what another can tell is where the session is');
$s->query_safe(q{SELECT test_branch_checkout('main')});

# A function's own setting of the branch: in use while it runs, undone as it returns.
$node->safe_psql('postgres', qq{SET catalog_branch = $main; CREATE FUNCTION app.on_feat() RETURNS boolean LANGUAGE sql
	SET catalog_branch = $feat AS 'SELECT test_branch_in_use($main) IS NOT NULL'});
my ($frc, $fout, $ferr) = $node->psql('postgres', qq{SET catalog_branch = $main; SELECT app.on_feat()});
if ($frc == 0)
{
	$s->query_safe(q{SELECT app.on_feat()});
	is($s->query_safe('SHOW catalog_branch'), $main, 'a function\'s own setting of the branch is undone as it returns');
	is(in_use($main) . in_use($feat), 'tf', 'and what another can tell is where the session is');
}
else
{
	like($ferr, qr/catalog_branch/, 'a function can\'t set the branch itself');
	pass('and so leaves nothing to undo');
}

# Reset: on the branch it resets to, and holding that.
$s->query_safe(q{SELECT test_branch_checkout('feat')});
$s->query_safe('DISCARD ALL');
is($s->query_safe('SHOW catalog_branch'), '0', 'DISCARD ALL puts the session where it started');
is(in_use($main) . in_use($feat), 'ff', 'holding nothing');
$s->query_safe(q{SELECT test_branch_checkout('main')});

# Refused in a transaction that changed a versioned schema; not in one that changed only a shared one.
$s->query_safe(q{BEGIN; INSERT INTO public.plain VALUES (1); SELECT test_branch_checkout('feat'); ROLLBACK});
pass('a transaction that wrote only to a shared schema checks out');
my ($rc, $out, $err) = $node->psql('postgres', qq{SET catalog_branch = $main; BEGIN; INSERT INTO app.t VALUES ('x');
	SELECT test_branch_checkout('feat')});
like($err, qr/cannot check out branch "feat" in a transaction that has changed a versioned schema/,
	'one that wrote to a versioned table is refused');
($rc, $out, $err) = $node->psql('postgres', qq{SET catalog_branch = $main; BEGIN; ALTER TABLE app.t ADD COLUMN w integer;
	SELECT test_branch_checkout('feat')});
like($err, qr/cannot check out branch "feat" in a transaction/, 'and one that changed a versioned table\'s definition');
($rc, $out, $err) = $node->psql('postgres', qq{SET catalog_branch = $main; BEGIN; CREATE TABLE app.gone (id integer);
	COMMIT; BEGIN; DROP TABLE app.gone; SELECT test_branch_checkout('feat')});
like($err, qr/cannot check out branch "feat" in a transaction/, 'and one that dropped a versioned table');
($rc, $out, $err) = $node->psql('postgres', qq{SET catalog_branch = $main; BEGIN;
	CREATE FUNCTION app.f() RETURNS integer LANGUAGE sql AS 'SELECT 1'; SELECT test_branch_checkout('feat')});
like($err, qr/cannot check out branch "feat" in a transaction/, 'and one that made a function in a versioned schema');
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_branch_checkout('nope')});
like($err, qr/branch "nope" not found/, 'a branch not there is refused');

# Any role checks out.
$node->safe_psql('postgres', q{CREATE ROLE plain_user LOGIN});
is($node->safe_psql('postgres', q{SET ROLE plain_user; DO $$BEGIN PERFORM test_branch_checkout('feat'); END$$; SELECT v FROM app.t}), 'feat',
	'a role with no powers checks out');

# Taken from its sessions: a checkout waits until that's done.
my $taker = $node->background_psql('postgres');
$taker->query_safe("BEGIN; SELECT test_branch_in_use($feat)");
my $w = $node->background_psql('postgres');
my $wpid = $w->query_safe('SELECT pg_backend_pid()');
my $tpid = $taker->query_safe('SELECT pg_backend_pid()');
$w->query_until(qr/started/, "\\echo started\nSELECT test_branch_checkout('feat');\n\\echo done\n");
ok($node->poll_query_until('postgres', qq{SELECT pg_blocking_pids($wpid) = ARRAY[$tpid]}),
	'a checkout waits while a change has the branch from its sessions');
$taker->query_safe('COMMIT');
$w->query_until(qr/done/, '');
is($w->query_safe('SHOW catalog_branch'), $feat, 'and then checks out');
$_->quit for ($taker, $w);
ok($node->poll_query_until('postgres', "SELECT NOT test_branch_in_use($feat)"), 'which, gone, holds nothing');

# Leaving: the hold goes with the session, and with one ended in the middle of a transaction.
$s->quit;
ok($node->poll_query_until('postgres', "SELECT NOT test_branch_in_use($main)"), 'a session gone holds nothing');
my $mid = $node->background_psql('postgres');
$mid->query_safe(q{SELECT test_branch_checkout('main')});
my $midpid = $mid->query_safe(q{BEGIN; SELECT test_branch_checkout('feat'); SELECT pg_backend_pid()});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($midpid)");
eval { $mid->quit };
ok($node->poll_query_until('postgres', "SELECT NOT test_branch_in_use($main) AND NOT test_branch_in_use($feat)"),
	'and one ended in the middle of a transaction');

# The database's default branch, and a connection's own.
$node->safe_psql('postgres', q{ALTER DATABASE postgres SET default_branch = 'feat'});
is($node->safe_psql('postgres', q{SELECT current_setting('catalog_branch') || ':' || test_branch_current() || ':' || v FROM app.t}),
	"$feat:feat:feat", 'a new session checks out the database\'s default branch');
{
	local $ENV{PGOPTIONS} = '-c default_branch=main';
	is($node->safe_psql('postgres', q{SELECT test_branch_current()}), 'main', 'or the one its connection names');
}
{
	local $ENV{PGOPTIONS} = '-c default_branch=nope';
	($rc, $out, $err) = $node->psql('postgres', 'SELECT 1');
	like($err, qr/branch "nope" not found/, 'a connection naming a branch not there is refused');
}
$node->safe_psql('postgres', q{ALTER DATABASE postgres RESET default_branch});
is($node->safe_psql('postgres', q{SELECT test_branch_current() IS NULL}), 't', 'with none, a session starts on no branch');

done_testing();
