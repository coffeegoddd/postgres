
# Copyright (c) 2026, PostgreSQL Global Development Group

# A read that takes no lock, of a branch moved and its chunks freed meanwhile.
# The pointer it read names a chunk that was made after the statement's
# snapshot and freed before the chunk is read: it reads what it found.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('vc_read_freed');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_commit('main', 'base');
});

my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe(q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('vc-branch-read-pointer', 'wait');
	SELECT injection_points_attach('vc-branch-read-chunk', 'wait');
});

# stopped with its statement's snapshot taken, before it reads the pointer
$reader->query_until(qr/reading/,
	qq{\\echo reading\nSELECT test_vc_log('main') <> '' AS read;\n});
$node->wait_for_event('client backend', 'vc-branch-read-pointer');

# a chunk made after that snapshot, which the pointer then names
$node->safe_psql('postgres', q{SELECT test_vc_commit('main', 'after the snapshot')});
$node->safe_psql('postgres', q{
	SELECT injection_points_detach('vc-branch-read-pointer');
	SELECT injection_points_wakeup('vc-branch-read-pointer');
});

# stopped with the pointer read, before it reads the chunk
$node->wait_for_event('client backend', 'vc-branch-read-chunk');
my $named = $node->safe_psql('postgres',
	q{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'vc/branch/main'});

# the branch moved on, and the chunk that named freed
$node->safe_psql('postgres', q{SELECT test_vc_commit('main', 'after the read')});
isnt(
	$node->safe_psql('postgres',
		q{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'vc/branch/main'}),
	$named,
	'the branch has moved on');
$node->safe_psql('postgres', q{SELECT test_cs_free_unheld('pages')});
is( $node->safe_psql('postgres', qq{SELECT test_cs_has('pages', decode('$named', 'hex'))}),
	'f',
	'the chunk the reader found is freed');

# nothing takes its entry or bytes while the reader's snapshot might read them
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
$node->safe_psql('postgres', 'VACUUM pg_history.chunks');

$node->safe_psql('postgres', q{
	SELECT injection_points_detach('vc-branch-read-chunk');
	SELECT injection_points_wakeup('vc-branch-read-chunk');
});
my $result = $reader->query('SELECT 1');
is($reader->{stderr}, '', 'the read reads what it found');
like($result, qr/^t\n/, 'a log');
$reader->quit;

# many at once, the same: a snapshot holds a chunk freed since, among those not
my $main = $node->safe_psql('postgres',
	q{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'vc/branch/main'});
my $many = $node->background_psql('postgres');
$many->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$many->query_safe('SELECT count(*) FROM pg_class');
$node->safe_psql('postgres', q{
	SELECT test_vc_commit('main', 'after that snapshot');
	SELECT test_cs_free_unheld('pages');
});
is( $node->safe_psql('postgres', qq{SELECT test_cs_has('pages', decode('$main', 'hex'))}),
	'f',
	'the branch chunk that snapshot could see is freed');
my $live = $node->safe_psql('postgres',
	q{SELECT encode(hash, 'hex') FROM pg_history.pointers WHERE name = 'vc/branch/main'});
like(
	$many->query_safe(
		qq{SELECT g[1] IS NOT NULL AND g[2] IS NOT NULL
		   FROM test_cs_get_many('pages', ARRAY[decode('$main', 'hex'), decode('$live', 'hex')]) g}),
	qr/^t$/m,
	'and a read of many under it finds it, among one not freed');
$many->quit;

# a later read, with a later snapshot, finds the branch as it is now
like($node->safe_psql('postgres', q{SELECT test_vc_log('main')}), qr/\S/, 'and one after it');

$node->stop;
done_testing();
