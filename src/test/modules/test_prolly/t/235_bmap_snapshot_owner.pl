# Copyright (c) 2026, PostgreSQL Global Development Group

# A snapshot's slot names the transaction that took it, so a crash between
# a keep's snapshots and its commit leaks no slot: the snapshot is listed
# by nobody after the restart, and a page sweep or the relation's next
# snapshot lets go of it.  A snapshot is kept while its transaction runs,
# and one taken by a transaction that rolls back is let go of the same way.
# A standby replays what is let go of.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_snapshot_owner');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_keep_size = '256MB'
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_snapshot_owner_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA ko VERSIONED;
	CREATE TABLE ko.t (id int PRIMARY KEY, v int);
	INSERT INTO ko.t SELECT g, 0 FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'c1');
	CREATE FUNCTION slots_free(rel oid) RETURNS int LANGUAGE plpgsql AS $$
	DECLARE ids int8[] := '{}';
	BEGIN
		LOOP
			BEGIN
				ids := ids || test_bmap_snapshot_take(rel);
			EXCEPTION WHEN program_limit_exceeded THEN
				EXIT;
			END;
		END LOOP;
		PERFORM test_bmap_snapshot_release(rel, id) FROM unnest(ids) id;
		RETURN cardinality(ids);
	END $$;
});
my $rel = sql(q{SELECT pg_relation_filenode('ko.t')});

sub kept
{
	my ($n) = @_;
	$n //= $node;
	return $n->safe_psql('postgres',
		qq{SELECT count(*) FROM test_bmap_snapshots($rel)});
}

# How many more snapshots the table can keep: taken until it refuses one,
# then let go of.
sub free_slots
{
	return sql(qq{SELECT slots_free($rel)});
}

is(kept(), '1', 'c1 is kept by one snapshot');
is(free_slots(), '15', 'fifteen more slots are free');

# A keep stopped after its snapshot is taken, before its commit: the
# snapshot is kept while its transaction runs, a sweep included; then a
# crash.
sub keep_then_crash
{
	my ($msg) = @_;
	my $s = $node->background_psql('postgres');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-snapshot-taken', 'wait')});
	$s->query_until(qr/keeping/, qq{
		\\echo keeping
		UPDATE ko.t SET v = v + 1 WHERE id = 1;
		SELECT dolt_commit('-am', '$msg');
	});
	$node->wait_for_event('client backend', 'bmap-snapshot-taken');
	my $during = kept();
	sql(q{SELECT freed FROM prolly_page_sweep()});
	my $swept = kept();
	# (a commit flushes the snapshot's record)
	sql('SELECT txid_current()');
	$node->stop('immediate');
	eval { $s->quit };
	$node->start;
	return ($during, $swept);
}

my ($during, $swept) = keep_then_crash('c2');
is($during, '2', 'a snapshot is kept while its transaction runs');
is($swept, '2', 'and a sweep meanwhile leaves it');
is(kept(), '1', 'after the crash, nobody keeps the snapshot of c2');
sql(q{SELECT freed FROM prolly_page_sweep()});
is(free_slots(), '15', 'a sweep let go of its slot');

# The table's next keep lets go of what the crash left, unswept.
sql(q{UPDATE ko.t SET v = 2 WHERE id = 2; SELECT dolt_commit('-am', 'c3')});
is(kept(), '2', 'c1 and c3 are kept');
keep_then_crash('c4');
sql(q{UPDATE ko.t SET v = 3 WHERE id = 3; SELECT dolt_commit('-am', 'c5')});
# (c1 is retired by c5's keep, let go of by the next)
is(kept(), '3', 'c1, c3 and c5 are kept after a crash in c4\'s keep');
is(free_slots(), '13', 'the next keep let go of the slot the crash left');
sql(q{UPDATE ko.t SET v = 4 WHERE id = 4; SELECT dolt_commit('-am', 'c6')});

# Snapshots taken by a transaction that rolls back, and by a subtransaction
# rolled back in one that commits, are listed by nobody, and the next
# snapshot taken lets go of them.
sql(qq{BEGIN; SELECT test_bmap_snapshot_take($rel); ROLLBACK;
	BEGIN; SAVEPOINT s; SELECT test_bmap_snapshot_take($rel);
	ROLLBACK TO s; COMMIT;});
is(kept(), '3', 'snapshots of transactions rolled back are listed by nobody');
is(free_slots(), '13', 'and the next snapshot taken let go of them');

# the standby lists every slot it has (it judges no transaction): as many
# as the primary keeps, once it has replayed what was let go of
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is(kept(), '3', 'c3, c5 and c6 are kept');
is(kept($standby), '3', 'the standby replayed the slots let go of');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM ko.t'), '200',
	'and reads the table');

# a hard reset gives the pages c6 kept back, after it all
sql(q{UPDATE ko.t SET v = 100; SELECT dolt_reset('--hard')});
is(sql(q{SELECT sum(v) FROM ko.t}), '11', 'a reset gives the kept pages back');

$standby->stop;
$node->stop;
done_testing();
