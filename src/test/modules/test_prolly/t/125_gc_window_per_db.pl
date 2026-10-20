# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection's window holds off appends to the store it collects, not to
# other databases' stores: the window takes that database's append lock,
# not one of the server's.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('gc_window');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
for my $db ('a', 'b')
{
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, 'CREATE EXTENSION test_prolly');
	$node->safe_psql($db, qq{SELECT test_chunk_put('$db before')});
}
$node->safe_psql('a', 'CREATE EXTENSION injection_points');

# Database a's collection, stopped in its window.
my $g = $node->background_psql('a');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-kept', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT kept FROM test_chunk_gc(false);
	\echo done
});
$node->poll_query_until('a', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-kept'}) or die 'the collection did not stop';

# Database b's appends go on.
is($node->safe_psql('b', q{SET statement_timeout = '20s';
	SELECT test_chunk_put('b during') IS NOT NULL}), 't',
	'another database appends while the window is held');

# Database a's wait for the window.
my $w = $node->background_psql('a');
$w->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_put('a during') IS NOT NULL AS put;
	\echo done
});
ok($node->poll_query_until('a', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE datname = 'a' AND wait_event = 'prolly journal index'
	AND query LIKE '%a during%'}),
	'the collected database\'s append waits for the window');

$node->safe_psql('a', q{SELECT injection_points_wakeup('prolly-gc-kept')});
$g->query_until(qr/done/, '');
$g->quit;
like($w->query_until(qr/done/, ''), qr/t/, 'and appends once it is over');
$w->quit;
is($node->safe_psql('a', q{SELECT test_chunk_has(test_content_hash('a during'))}), 't',
	'its chunk is stored');
is($node->safe_psql('b', q{SELECT test_chunk_has(test_content_hash('b during'))}), 't',
	'and the other database\'s');

$node->stop;
done_testing();
