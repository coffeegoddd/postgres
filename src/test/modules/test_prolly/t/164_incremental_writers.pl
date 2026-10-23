# Copyright (c) 2026, PostgreSQL Global Development Group

# A fold that works out again only the tables written since the last one
# learns which from the refs, which hear of a writer's relations as it
# ends, a moment after others already see its commit.  A fold in that
# moment, by a snapshot that sees the commit, must not take the last
# tree's items: it folds everything, and so has the row.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('incremental_writers');
$node->init;
# (unchecked, so a fold that missed the row gives another tree, not an
# error)
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
debug_prolly_verify_fold_caches = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	CREATE TABLE s.u (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'old');
	INSERT INTO s.u VALUES (1, 'u');
	SELECT dolt_commit('-Am', 'base');
});

# the reader's last tree, made by a fold of everything, then one of only
# what changed
my $reader = $node->background_psql('postgres');
$reader->query_safe(q{UPDATE s.u SET v = 'u1'});
$reader->query_safe(q{SELECT test_fold_root()});
$reader->query_safe(q{UPDATE s.u SET v = 'u2'});
$reader->query_safe(q{SELECT test_fold_root()});
$reader->query_safe(q{SELECT pg_stat_force_next_flush()});
ok( $reader->query_safe(
		q{SELECT value > 0 FROM pg_stat_prolly WHERE name = 'fold_incremental'})
	  eq 't',
	'the reader folds only what changed');

# a writer whose commit others see, paused before the refs hear of it
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{SELECT injection_points_set_local()});
$writer->query_safe(
	q{SELECT injection_points_attach('prolly-writes-ending', 'wait')});
$writer->query_until(qr/started/, q{
	\echo started
	UPDATE s.t SET v = 'new' WHERE id = 1;
	\echo done
});
$node->wait_for_event('client backend', 'prolly-writes-ending');

is($reader->query_safe(q{SELECT v FROM s.t WHERE id = 1}),
	'new', 'the reader sees the commit');
is( $reader->query_safe(
		q{SELECT test_fold_root() = test_fold_root(true)}),
	't', 'its fold has the row, as a fold of everything does');

sql(q{SELECT injection_points_wakeup('prolly-writes-ending')});
$writer->query_until(qr/done/, '');
$writer->query_safe(
	q{SELECT injection_points_detach('prolly-writes-ending')});

# and once the refs have heard, the next fold is of only what changed again
is( $reader->query_safe(
		q{SELECT test_fold_root() = test_fold_root(true)}),
	't', 'after the writer ends, the same');

$writer->quit;
$reader->quit;
$node->stop;
done_testing();
