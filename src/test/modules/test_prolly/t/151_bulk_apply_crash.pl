# Copyright (c) 2026, PostgreSQL Global Development Group

# A move's rows inserted many at once, and a table it fills from nothing
# with its indexes built after the rows (#490), across a crash: one cut
# off in the middle leaves the branch as it was, and one committed keeps
# every row and a valid index of each, whether the rows' WAL was written
# (wal_level replica) or skipped and the new storage synced or logged at
# commit (minimal).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

for my $wal_level ('replica', 'minimal')
{
	my $node = PostgreSQL::Test::Cluster->new("bulk_$wal_level");
	$node->init;
	$node->append_conf('postgresql.conf', qq{
wal_level = $wal_level
max_wal_senders = 0
autovacuum = off
checkpoint_timeout = '1h'
});
	$node->start;

	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	my $crash = sub { $node->stop('immediate'); $node->start; };

	# Every btree index of the schema's tables checked, with every row of
	# its table found in it; and the rows, read by a sequential scan.
	my $valid = sub {
		return $sql->(q{
			SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
			FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
			JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
			WHERE i.indrelid IN (SELECT oid FROM pg_class
								 WHERE relnamespace = 'bk'::regnamespace)});
	};
	my $rows = sub {
		return $sql->(qq{
			SET enable_indexscan = off;
			SET enable_bitmapscan = off;
			SET enable_indexonlyscan = off;
			SELECT count(*) || ':' || coalesce(sum($_[1]), 0) FROM $_[0]});
	};

	$sql->(q{
		CREATE EXTENSION amcheck;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA bk VERSIONED;
		CREATE TABLE bk.t (id int PRIMARY KEY, v text, u int UNIQUE);
		CREATE INDEX t_v ON bk.t (v);
		CREATE TABLE bk.k (a int, b text);
		SELECT dolt_commit('-Am', 'empty');
	});
	my $c0 = $sql->(q{SELECT dolt_hashof('HEAD')});
	$sql->(q{
		INSERT INTO bk.t SELECT g, 'v' || g, g FROM generate_series(1, 20000) g;
		INSERT INTO bk.k SELECT g % 100, 'k' FROM generate_series(1, 3000) g;
		SELECT dolt_commit('-am', 'rows');
	});
	my $c1 = $sql->(q{SELECT dolt_hashof('HEAD')});
	$sql->(q{
		CREATE TABLE bk.n (id int PRIMARY KEY, v text UNIQUE);
		CREATE INDEX n_v ON bk.n (v DESC);
		INSERT INTO bk.n SELECT g, 'n' || g FROM generate_series(1, 20000) g;
		SELECT dolt_commit('-Am', 'made');
	});
	my $c2 = $sql->(q{SELECT dolt_hashof('HEAD')});
	$sql->("SELECT dolt_reset('--hard', '$c0')");
	$sql->('CHECKPOINT');

	# A move stopped after its first rows are in, and the server crashed.
	my $cut_off = sub {
		my ($target) = @_;
		my $s = $node->background_psql('postgres', on_error_stop => 0);
		$s->query_safe(q{SELECT injection_points_set_local();
			SELECT injection_points_attach('prolly-insert-many', 'wait')});
		$s->query_until(qr/moving/, qq{
			\\echo moving
			SELECT dolt_reset('--hard', '$target');
		});
		$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE wait_event = 'prolly-insert-many'})
		  or die 'the move did not wait';
		$crash->();
		$s->quit;
	};

	# Into tables that were there.
	$cut_off->($c1);
	is($rows->('bk.t', 'id') . ',' . $rows->('bk.k', 'a'), '0:0,0:0',
		"$wal_level: a move cut off by a crash leaves no rows of it");
	is($sql->('SELECT count(*) FROM dolt_status'), '0',
		"$wal_level: and the branch as it was");
	is($valid->(), 't', "$wal_level: its indexes valid");
	$sql->("SELECT dolt_reset('--hard', '$c1')");
	is($rows->('bk.t', 'id') . ',' . $rows->('bk.k', 'a'), '20000:200010000,3000:148500',
		"$wal_level: the move made again writes every row");
	is($valid->(), 't', "$wal_level: every index valid");

	# Into a table the move makes, loaded.
	$cut_off->($c2);
	is($sql->(q{SELECT to_regclass('bk.n') IS NULL}), 't',
		"$wal_level: a load cut off by a crash leaves no table");
	is($sql->('SELECT count(*) FROM dolt_status'), '0',
		"$wal_level: and the branch as it was");

	# Committed, then a crash before any checkpoint: with minimal WAL, the
	# new storage synced at commit (a threshold of 0), or logged whole.
	for my $threshold ('0', '1GB')
	{
		$sql->("SELECT dolt_reset('--hard', '$c1')");
		$sql->('CHECKPOINT');
		$sql->(qq{
			SET wal_skip_threshold = '$threshold';
			SELECT dolt_reset('--hard', '$c2');
		});
		$crash->();
		is($rows->('bk.n', 'id'), '20000:200010000',
			"$wal_level, threshold $threshold: a committed load keeps its rows");
		is($valid->(), 't', "$wal_level, threshold $threshold: and valid indexes");
		is($sql->(q{
			SET enable_seqscan = off;
			SELECT id FROM bk.n WHERE v = 'n12345'}), '12345',
			"$wal_level, threshold $threshold: which find the rows");
		is($sql->('SELECT count(*) FROM dolt_status'), '0',
			"$wal_level, threshold $threshold: the branch at the commit");
	}
	$node->stop;
}

done_testing();
