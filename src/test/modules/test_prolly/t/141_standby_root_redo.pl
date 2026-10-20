# Copyright (c) 2026, PostgreSQL Global Development Group

# A standby replays a commit's move of the chunk store's root (its ROOT
# record over the PAD the commit reserved) while the transaction is still
# running to its backends: one that reads the journal meanwhile keeps the
# PAD as in flight, and then reads the root that took its place, rather
# than drop the PAD as an ended transaction's and miss the root.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('root_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres', 'CREATE DATABASE sr');
$primary->safe_psql('sr', 'CREATE EXTENSION test_prolly');
my $a = $primary->safe_psql('sr', q{SELECT test_chunk_put('root a')});
$primary->safe_psql('sr', "SELECT test_chunk_move_root(NULL, '$a')");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('root_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('sr', 'SELECT test_chunk_root()'), $a,
	'the standby reads the root');

# The standby's replay held just before the root's move.
$standby->safe_psql('postgres',
	q{SELECT injection_points_attach('prolly-commit-root-redo', 'wait')});
my $b = $primary->safe_psql('sr', q{SELECT test_chunk_put('root b')});
$primary->safe_psql('sr', "SELECT test_chunk_move_root('$a', '$b')");
$standby->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-commit-root-redo'})
  or die 'the replay did not wait';

# Backends read the journal meanwhile: the root hasn't moved yet.  (The
# first read finds the PAD; the next decides whether to keep it.)
my $reader = $standby->background_psql('sr');
is($reader->query_safe('SELECT test_chunk_root()'), $a,
	'read as the move replays, the root is the old one');
is($standby->safe_psql('sr', 'SELECT test_chunk_root()'), $a,
	'... read again, still');

$standby->safe_psql('postgres',
	q{SELECT injection_points_detach('prolly-commit-root-redo')});
$standby->safe_psql('postgres',
	q{SELECT injection_points_wakeup('prolly-commit-root-redo')});
$primary->wait_for_replay_catchup($standby);
is($reader->query_safe('SELECT test_chunk_root()'), $b,
	'once it has replayed, the same backend reads the new root');
is($standby->safe_psql('sr', 'SELECT test_chunk_root()'), $b,
	'... as a new one does');
$reader->quit;

$standby->stop;
$primary->stop;
done_testing();
