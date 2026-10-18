# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit in flight leaves a PAD in the journal, room for its ROOT record.
# A session reading the store's root meanwhile notes the PAD, to see again
# whether it became a ROOT; it keeps that note from one transaction to the
# next, and reads it again in its next transaction.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('journal_pads');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', q{
	CREATE SCHEMA jp VERSIONED;
	CREATE TABLE jp.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'base');
});
my $base = $node->safe_psql('postgres', q{SELECT dolt_hashof('main')});

SKIP:
{
	skip 'injection points not supported by this build', 4
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

	# A commit in flight: its PAD written, its ROOT not yet.
	my $writer = $node->background_psql('postgres');
	$writer->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-root-padded', 'wait')});
	$writer->query_until(qr/started/, q{
		\\echo started
		INSERT INTO jp.t VALUES (1);
		SELECT dolt_commit('-am', 'in flight');
		\\echo done
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-root-padded'}) or die 'the commit did not wait';

	# A reader sees the root, in one transaction and the next, and the next.
	my $reader = $node->background_psql('postgres');
	for my $i (1 .. 3)
	{
		is($reader->query_safe(q{SELECT dolt_hashof('main')}), $base,
			"a reader sees the root before the commit, transaction $i");
	}

	$node->safe_psql('postgres', q{SELECT injection_points_wakeup('prolly-root-padded')});
	$writer->query_until(qr/done/, '');
	isnt($reader->query_safe(q{SELECT dolt_hashof('main')}), $base,
		'and the commit once it is');
	$writer->quit;
	$reader->quit;
}

$node->stop;
done_testing();
