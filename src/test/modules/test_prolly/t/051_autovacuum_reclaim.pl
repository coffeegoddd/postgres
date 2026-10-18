# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum reclaims prolly's stores: it collects the chunk store once
# its journal segment passes prolly.gc_journal_threshold (and the settled
# files' size times prolly.gc_growth_scale_factor), not before, nor with
# the threshold -1; and it sweeps a page store after the server starts,
# so blocks a crash leaked are freed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('autovac');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = 1MB
prolly.gc_growth_scale_factor = 0
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE av');
$node->safe_psql('av', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('av', $_[0]); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

is(sql('SHOW prolly.gc_journal_threshold'), '1MB', 'the threshold is a setting');

# Below the threshold, garbage stays.
my $dead = sql(q{SELECT test_chunk_put('dead below the threshold')});
sleep(3);
is(has($dead), 't', 'garbage below the threshold stays');

# Past it, autovacuum collects it.
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 100000), 'UTF8')))
	FROM generate_series(1, 20) g});
ok($node->poll_query_until('av', qq{SELECT NOT test_chunk_has('$dead')}),
	'past the threshold, autovacuum collects the garbage');

# With the threshold -1, it doesn't.
$node->append_conf('postgresql.conf', 'prolly.gc_journal_threshold = -1');
$node->reload;
$node->poll_query_until('av', q{SELECT current_setting('prolly.gc_journal_threshold') = '-1'})
  or die 'the setting did not reload';
my $dead2 = sql(q{SELECT test_chunk_put('dead with collection off')});
sql(q{SELECT count(test_chunk_put(convert_to(repeat(g::text, 100001), 'UTF8')))
	FROM generate_series(1, 20) g});
sleep(3);
is(has($dead2), 't', 'with the threshold -1, nothing is collected');

# A crash leaks a dropped table's blocks; after the restart, autovacuum
# sweeps the page store and frees them.
sql(q{CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.d (a int8);
	INSERT INTO cow.d SELECT g FROM generate_series(1, 5000) g});
my $dblocks = sql(q{SELECT pg_relation_size('cow.d') / 8192});
# blocks allocated: the store's length less its free blocks
my $allocated = q{SELECT (pg_stat_file(format('base/%s/9421',
	(SELECT oid FROM pg_database WHERE datname = 'av')))).size / 8192 - test_bmap_free_blocks()};
my $before = sql($allocated);
my $d = $node->background_psql('av', on_error_stop => 0);
$d->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-predrop', 'wait')});
$d->query_until(qr/started/, q{
	\echo started
	DROP TABLE cow.d;
});
$node->poll_query_until('av', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-predrop'}) or die 'the drop did not stop';
$node->stop('immediate');
eval { $d->quit; };
$node->start;
ok($node->poll_query_until('av', qq{SELECT ($allocated) <= $before - $dblocks}),
	'after a crash, autovacuum sweeps the page store and frees what it leaked');

$node->stop;
done_testing();
