# Copyright (c) 2026, PostgreSQL Global Development Group

# A session checked out at a commit is on a hidden branch there, read-only,
# that every session detached at the commit shares.  No other session moves
# it: a connection doesn't name it ("dbname/detached/<hash>"), as
# dolt_checkout() doesn't, and dolt_branch -m doesn't rename it, which would
# leave a writable branch where the detached sessions are.  Nor does a push
# move it (dolt_serve_move_head(), as dolt_serve_heads() doesn't list it), nor
# a clone make one from a remote's branch so named.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('detached_hidden');
$node->init;
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql(q{CREATE SCHEMA d VERSIONED;
	CREATE TABLE d.t (id int PRIMARY KEY);
	INSERT INTO d.t VALUES (1)});
my $c1 = sql(q{SELECT (dolt_commit('-Am', 'c1'))[1]});

# Session A, detached at c1, stays on its hidden branch.
my $a = $node->background_psql('postgres');
$a->query_safe(q{SELECT dolt_checkout(dolt_hashof('HEAD'))});
my $hidden = $a->query_safe(q{SELECT active_branch()});
is($hidden, 'detached/' . substr($c1, 0, 40), 'A is on the commit\'s hidden branch');

# A connection naming it is refused, so it writes nothing there.
my ($ret, $stdout, $stderr) = $node->psql("postgres/$hidden",
	q{SELECT current_setting('transaction_read_only');
	INSERT INTO d.t VALUES (2);
	SELECT dolt_commit('-Am', 'on hidden')});
isnt($ret, 0, 'a connection to a hidden branch is refused');
like($stderr, qr/branch names beginning "detached\/" are reserved/,
	'as its name is to dolt_checkout');
is($stdout, '', 'it ran nothing');

# Nor does another session rename it, forced or not.
for my $flags ('-m', '-mf')
{
	($ret, $stdout, $stderr) = $node->psql('postgres',
		"SELECT dolt_branch('$flags', '$hidden', 'zz')");
	like($stderr, qr/a commit's hidden branch can't be renamed/,
		"dolt_branch $flags doesn't rename a hidden branch");
}
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'zz'}), '0',
	'no branch zz');

# Nor does a push move it, nor delete it: dolt_serve_move_head() refuses its
# name, as dolt_serve_heads() doesn't list it.
sql(q{INSERT INTO d.t VALUES (2)});
my $c2 = sql(q{SELECT (dolt_commit('-Am', 'c2'))[1]});
for my $head ("decode('$c2', 'hex')", 'NULL')
{
	($ret, $stdout, $stderr) = $node->psql('postgres',
		qq{SELECT dolt_serve_move_head('$hidden', decode('$c1', 'hex'),
			$head, dolt_serve_format(), false)});
	like($stderr, qr/branch names beginning "detached\/" are reserved/,
		"dolt_serve_move_head refuses a hidden branch (head $head)");
	is($stdout, '', 'and moves nothing');
}

# A session doesn't push a hidden branch (its own, or one of its name) to a
# remote, where a clone would make it.
my $url = 'file://' . PostgreSQL::Test::Utils::tempdir() . '/origin';
sql(qq{SELECT dolt_remote('add', 'origin', '$url')});
($ret, $stdout, $stderr) = $node->psql('postgres',
	qq{SELECT dolt_checkout('$c1'); SELECT dolt_push('origin', '$hidden')});
like($stderr, qr/branch names beginning "detached\/" are reserved/,
	'dolt_push refuses a hidden branch');
sql(q{SELECT dolt_push('origin', 'main')});

# A remote's branch named as c1's hidden branch, at c2 (a file remote holds
# any name): a clone tracks it but makes no branch of it, so a checkout of c1
# in the clone finds c1.
my $manifest = ($url =~ s{^file://}{}r) . '/prolly_remote';
PostgreSQL::Test::Utils::append_to_file($manifest, "head $c2 $hidden\n");
sql('CREATE DATABASE cl');
$node->safe_psql('cl', 'CREATE EXTENSION test_prolly');
($ret, $stdout, $stderr) = $node->psql('cl',
	qq{SELECT dolt_clone('$url', '--branch', '$hidden')});
like($stderr, qr/branch names beginning "detached\/" are reserved/,
	'a clone doesn\'t check out a remote\'s hidden branch');
($ret, $stdout, $stderr) = $node->psql('cl', qq{SELECT dolt_clone('$url')});
is($ret, 0, 'a clone of a remote with a branch named as a hidden one succeeds');
like($stderr, qr/remote branch "\Q$hidden\E" isn't cloned as a branch/,
	'but doesn\'t make that branch');
is($node->safe_psql('cl', qq{SELECT dolt_hashof('origin/$hidden') = '$c2'}),
	't', 'it is tracked');
is($node->safe_psql('cl', qq{SELECT (dolt_checkout('$c1'))[2];
	SELECT string_agg(id::text, ',') FROM d.t}),
	"HEAD is now at $c1 (detached, read-only)\n1",
	'a checkout of c1 in the clone is at c1');

# A is where it was: at c1, on its hidden branch, read-only, seeing c1's rows.
is($a->query_safe(q{SELECT active_branch() || ' ' || (dolt_hashof('HEAD') = '} . $c1
	  . q{') || ' ' || current_setting('transaction_read_only')
	  || ' ' || (SELECT string_agg(id::text, ',') FROM d.t)}),
	"$hidden true on 1", 'A is still detached at c1, read-only');

# Another session detached at c1 shares the branch, still at the commit.
is(sql(q{SELECT (dolt_checkout('} . $c1 . q{'))[2]}),
	"HEAD is now at $c1 (detached, read-only)",
	'a later checkout of c1 finds its hidden branch at it');

$a->quit;
$node->stop;
done_testing();
