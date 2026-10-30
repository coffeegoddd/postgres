# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash between the records of each step of block-mapped storage that
# takes more than one: a share, a copy of a shared page's path, a drop, a
# truncation, a sweep and a compaction's move.  The server is stopped at
# once while a backend waits at an injection point between two of the
# step's records.  Recovery must leave a consistent map: every table reads
# its rows, a sweep frees only blocks nothing holds (new rows written over
# whatever it freed change no table's rows), a second sweep finds nothing
# more, and a standby reads what the primary does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('crash_steps');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
restart_after_crash = off
});
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('crash_steps_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
});

# The tables whose rows each crash must leave as they were, by name, and
# copies made with the helpers, by file number.
my %tables;
my %copies;

sub table_rows
{
	my ($node, $name) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT md5(coalesce(string_agg(t::text, ',' ORDER BY t::text), '')) FROM $name t});
}

sub remember_table
{
	my ($name) = @_;
	$tables{$name} = table_rows($primary, $name);
}

sub remember_copy
{
	my ($filenode) = @_;
	$copies{$filenode} = sql(qq{SELECT test_bmap_rows_digest($filenode)});
}

sub make_table
{
	my ($name, $rows) = @_;
	sql(qq{
		CREATE TABLE $name (a int, b text);
		INSERT INTO $name SELECT g, '$name ' || g FROM generate_series(1, $rows) g;
	});
	remember_table($name);
	return sql(qq{SELECT pg_relation_filenode('$name')});
}

# Every table and copy reads its rows.
sub check_rows
{
	my ($node, $what) = @_;
	my $bad = 0;

	for my $name (sort keys %tables)
	{
		my $rows = table_rows($node, $name);
		if ($rows ne $tables{$name})
		{
			diag("table $name reads other rows $what");
			$bad++;
		}
	}
	for my $filenode (sort keys %copies)
	{
		my $rows = $node->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($filenode)});
		if ($rows ne $copies{$filenode})
		{
			diag("copy $filenode reads other rows $what");
			$bad++;
		}
	}
	return $bad == 0;
}

# Crash while a backend running 'statement' waits at 'point', then check
# the map recovery leaves.  'dropping' names a table the statement drops,
# if it does: gone if its drop committed before the crash.
my $round = 0;

sub crash_at
{
	my ($step, $point, $statement, $dropping) = @_;

	$round++;
	sql(qq{SELECT injection_points_attach('$point', 'wait')});
	my $session = $primary->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/started/, qq{
		\\echo started
		$statement;
	});
	$primary->wait_for_event('client backend', $point);
	$primary->stop('immediate');
	$session->quit;
	$primary->start;

	delete $tables{$dropping}
	  if defined $dropping
	  && sql(qq{SELECT to_regclass('$dropping') IS NULL}) eq 't';
	ok(check_rows($primary, "after a crash in $step"),
		"$step: every table reads its rows after the crash");
	sql(q{SELECT * FROM test_bmap_release_drain()});
	sql(q{SELECT * FROM test_bmap_sweep()});
	ok(check_rows($primary, "after the sweep following a crash in $step"),
		"$step: and after a drain and a sweep");

	# New rows written over whatever was freed.
	sql(qq{
		CREATE TABLE filler_$round (a int, b text);
		INSERT INTO filler_$round SELECT g, repeat('f', 200) FROM generate_series(1, 3000) g;
	});
	remember_table("filler_$round");
	ok(check_rows($primary, "once freed blocks were reused after a crash in $step"),
		"$step: and once the freed blocks are written again");
	is(sql(q{SELECT freed FROM test_bmap_sweep()}), '0',
		"$step: a second sweep frees nothing");

	$primary->wait_for_replay_catchup($standby);
	ok(check_rows($standby, "on the standby after a crash in $step"),
		"$step: the standby reads the same rows");
}

# Tables that share pages, before any crash.
my $kept = make_table('kept', 5000);
my $kept_copy = sql(qq{SELECT test_bmap_share($kept)});
remember_copy($kept_copy);
sql(q{UPDATE kept SET b = b || ' changed' WHERE a % 4 = 0});
remember_table('kept');

# A share, between its tree's record and the record raising the source's
# epoch: the copy's map is made but never named.
my $shared = make_table('shared', 4000);
crash_at('a share', 'bmap-share-map-page', qq{SELECT test_bmap_share($shared)});

# A copy of a shared page's path, between the copied node's record and the
# page's own copy: the transaction never commits.
my $copied = make_table('copied', 4000);
my $copied_copy = sql(qq{SELECT test_bmap_share($copied)});
remember_copy($copied_copy);
crash_at('a copy', 'bmap-tree-copied', q{UPDATE copied SET b = 'never' WHERE a = 2000});

# A drop, after its commit, between the records queuing its blocks and
# the release of them: the blocks are let go of once.
my $dropped = make_table('dropped', 4000);
crash_at('a drop', 'bmap-dropped', q{DROP TABLE dropped}, 'dropped');
is(sql(q{SELECT to_regclass('dropped') IS NULL}), 't', 'a drop: the drop committed');

# A truncation, between the records letting go of the blocks past the new
# end and those freeing them: the blocks are left to a sweep.
make_table('truncated', 6000);
sql(q{DELETE FROM truncated WHERE a > 1000});
remember_table('truncated');
crash_at('a truncation', 'bmap-truncation-let-go', q{VACUUM truncated});

# A sweep, between its counting and its freeing.
my $gone = make_table('gone', 4000);
sql(q{DROP TABLE gone});
delete $tables{gone};
crash_at('a sweep', 'bmap-sweep-freeing', q{SELECT * FROM test_bmap_sweep()});

# A compaction, between a page's move and the freeing of its old block:
# blocks held by a root let go of leave room below the table's.
my $low = make_table('low', 3000);
my $root = sql(qq{SELECT test_bmap_root_capture($low)});
sql(q{DROP TABLE low});
delete $tables{low};
sql(q{SELECT * FROM test_bmap_release_drain()});
make_table('high', 3000);
sql(qq{SELECT test_bmap_root_release($root)});
sql(q{SELECT * FROM test_bmap_release_drain()});
crash_at('a compaction', 'bmap-compact-moved', q{SELECT * FROM test_bmap_sweep()});

$standby->stop;
$primary->stop;
done_testing();
