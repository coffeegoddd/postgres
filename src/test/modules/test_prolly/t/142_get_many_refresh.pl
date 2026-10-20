# Copyright (c) 2026, PostgreSQL Global Development Group

# get_many() locates every chunk before reading any; one that misses has
# the store read the journal and its table files' list again, which may
# free a table file a conjoin replaced meanwhile.  The chunks located
# before are located again after, from the files as they are then: each
# reads back whole.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('get_many_refresh');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE DATABASE gm');
$node->safe_psql('gm', 'CREATE EXTENSION test_prolly');
sub sql { return $node->safe_psql('gm', $_[0]); }
sub injection { return $node->safe_psql('postgres', $_[0]); }

# x in one table file, y in another; z not stored yet.
sql(q{SELECT test_chunk_table_file(ARRAY[test_noise('x', 3000)])});
sql(q{SELECT test_chunk_table_file(ARRAY[test_noise('y', 3000)])});
my $want = q{ARRAY[test_content_hash(test_noise('x', 3000)),
	test_content_hash(test_noise('z', 3000))]};

# A backend that has the files open asks for x and z: x is located, z
# misses, and it waits before reading the store again.
my $a = $node->background_psql('gm');
is($a->query_safe(q{SELECT test_chunk_has(test_content_hash(test_noise('x', 3000)))}),
	't', 'the backend has the files open');
injection(q{SELECT injection_points_attach('prolly-get-many-refreshing', 'wait')});
$a->query_until(qr/started/, qq{
	\\echo started
	SELECT string_agg(i || ':' || (data IS NOT NULL
		AND data = CASE i WHEN 1 THEN test_noise('x', 3000) ELSE test_noise('z', 3000) END),
		',' ORDER BY i) AS got
	FROM test_chunk_get_many($want);
	\\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-get-many-refreshing'}) or die 'get_many did not wait';

# Meanwhile z is stored, and the two files conjoined into one.
sql(q{SELECT test_chunk_put(test_noise('z', 3000)) IS NOT NULL});
sql('SELECT test_chunk_conjoin()');
is(sql('SELECT count(*) FROM test_chunk_table_files()'), '1', 'the files conjoined');

injection(q{SELECT injection_points_detach('prolly-get-many-refreshing')});
injection(q{SELECT injection_points_wakeup('prolly-get-many-refreshing')});
my $out = $a->query_until(qr/done/, '');
like($out, qr/1:true,2:true/, 'both read back whole, from the files as they are now');
$a->quit;

$node->stop;
done_testing();
