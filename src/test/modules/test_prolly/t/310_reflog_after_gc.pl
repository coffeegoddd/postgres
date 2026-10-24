# Copyright (c) 2026, PostgreSQL Global Development Group

# dolt_reflog after a garbage collection lists only the changes of refs
# since the root the collected segments ended at.  A root move that changes
# no ref (the fold worker's checkpoint of a written branch) lists nothing,
# read by another session.  And the root a move in flight as the collection
# sealed the journal went to is where the changes begin: a move after the
# collection that undoes it is listed.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reflog_after_gc');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = on
prolly_fold_worker_naptime = 50ms
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA rg VERSIONED;
	CREATE TABLE rg.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'base');
	INSERT INTO rg.t VALUES (1);
	SELECT dolt_commit('-am', 'row 1');
});
is(sql(q{SELECT count(*) FROM dolt_reflog('main')}), '2', 'two moves of main');

# The fold worker's checkpoint after the collection moves the root, and no ref.
sql(q{SELECT dolt_gc()});
my $offset = -s $node->logfile;
sql(q{INSERT INTO rg.t VALUES (2)});
$node->wait_for_log(qr/fold worker folded branch/, $offset);
is(sql(q{SELECT count(*) FROM dolt_reflog('main')}), '0',
	"the worker's checkpoint after a collection lists nothing");
sql(q{DELETE FROM rg.t WHERE id = 2});

SKIP:
{
	skip 'injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';
	sql('CREATE EXTENSION injection_points');
	my $before = sql(q{SELECT dolt_hashof('main')});

	# A commit in flight: its PAD reserved in the segment the collection seals.
	my $writer = $node->background_psql('postgres');
	$writer->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-root-padded', 'wait')});
	$writer->query_until(qr/started/, q{
		\\echo started
		INSERT INTO rg.t VALUES (3);
		SELECT dolt_commit('-am', 'in flight');
		\\echo done
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-root-padded'}) or die 'the commit did not wait';

	# The collection seals the journal, then waits for the commit.
	my $gc = $node->background_psql('postgres');
	$gc->query_until(qr/collecting/, q{
		\\echo collecting
		SELECT dolt_gc();
		\\echo collected
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE query LIKE 'SELECT dolt_gc()%' AND wait_event IS NOT NULL
		  AND wait_event <> 'ClientRead'}) or die 'the collection did not wait';
	sql(q{SELECT injection_points_detach('prolly-root-padded');
		SELECT injection_points_wakeup('prolly-root-padded')});
	$writer->query_until(qr/done/, '');
	$gc->query_until(qr/collected/, '');
	$writer->quit;
	$gc->quit;
	isnt(sql(q{SELECT dolt_hashof('main')}), $before, 'the commit moved main');

	# main moved back, after the collection: a change from where it was.
	sql(qq{SELECT dolt_reset('--hard', '$before')});
	is(sql(q{SELECT commit_message FROM dolt_reflog('main')}), 'row 1',
		'a move after the collection undoing the move in flight is listed');
}

$node->stop;
done_testing();
