# Copyright (c) 2026, PostgreSQL Global Development Group

# A session checked out at a commit is on a hidden branch there, read-only,
# that every session detached at the commit shares.  No other session moves
# it: a connection doesn't name it ("dbname/detached/<hash>"), as
# dolt_checkout() doesn't, and dolt_branch -m doesn't rename it, which would
# leave a writable branch where the detached sessions are.

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
