# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery replays records of block-mapped storage that is dropped
# later in the WAL, whose map is gone from disk.  An earlier record's image
# of the map's header makes the map's file again (redo makes a missing file
# to restore an image in), with no root page.  The relation stays
# block-mapped: replay of its records skips them, noting their pages
# invalid, and the drop's replay forgets them, so recovery completes; it
# doesn't fail looking up the relation's pages in a map with no root
# ("could not read blocks 1..1").  Images of its pages restored to buffers
# go nowhere when written out.  A record of a node of the dropped map, whose
# block was freed and made another relation's page since, leaves that page
# alone (with full_page_writes off, nothing else restores it).  A live
# relation whose map lost its root page is treated the same, so an archive
# or standby recovery fails reaching consistency: its records' pages stay
# invalid.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

sub map_path
{
	my ($node, $rel) = @_;
	return $node->data_dir . '/'
	  . $node->safe_psql('postgres', qq{SELECT pg_relation_filepath('$rel')})
	  . '_bmap';
}

sub run_case
{
	my ($fpw) = @_;
	my $name = "dropped_map_fpw_$fpw";
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf(
		'postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
full_page_writes = $fpw
shared_buffers = 64MB
log_min_messages = debug1
});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	$sql->('CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache');

	# s: extended and changed after a checkpoint (its header's image logged
	# first), then dropped, its blocks freed and taken by t, whose pages
	# are written out before a crash.
	$sql->(q{CREATE TABLE s (a int, b text) WITH (fillfactor = 20);
		INSERT INTO s SELECT g, repeat('s', 200) FROM generate_series(1, 100) g});
	$sql->('CHECKPOINT');
	my $spath = map_path($node, 's');
	my $srel = $sql->(q{SELECT pg_relation_filepath('s')});
	open(my $fh, '<:raw', $spath) or die "open $spath: $!";
	read($fh, my $header, 8192) == 8192 or die "short read";
	close($fh);
	$sql->(q{INSERT INTO s SELECT g, repeat('s', 200) FROM generate_series(101, 3000) g;
		UPDATE s SET b = 'u' WHERE a % 5 = 0});
	$sql->(q{DROP TABLE s; SELECT * FROM test_bmap_release_drain()});
	$sql->(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 20);
		INSERT INTO t SELECT g, repeat('t', 200) FROM generate_series(1, 3000) g});
	my $want = $sql->(q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM t});
	my $trel = $sql->(q{SELECT pg_relation_filenode('t')});
	$sql->(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $trel
		AND (pg_buffercache_evict(bufferid)).buffer_evicted});
	ok(!-e $spath, "fpw $fpw: s's map is gone");
	$node->stop('immediate');

	# (s's map as an earlier record's image of its header makes it again)
	open($fh, '>:raw', $spath) or die "open $spath: $!";
	print $fh $header or die "write: $!";
	close($fh) or die "close: $!";
	my $log_from = -s $node->logfile;
	ok($node->start(fail_ok => 1), "fpw $fpw: recovery replays s's records");
	my $log = substr(slurp_file($node->logfile), $log_from);
	unlike($log, qr/could not read blocks|invalid page in block/,
		"fpw $fpw: without failing on s's map or the blocks t took");
	like($log, qr/page \d+ of relation \Q$srel\E does not exist/,
		"fpw $fpw: s's pages are noted invalid");
	is($sql->(q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM t}),
		$want, "fpw $fpw: t reads its rows");
	ok(!-e $spath, "fpw $fpw: and s's map is gone again");

	$node->stop;
}

run_case('on');
run_case('off');

# A live map that lost its root page, on a standby restarting from a
# restartpoint before a change of the relation: the relation stays
# block-mapped, its record's page is noted invalid, and recovery fails
# reaching consistency, as for a relation whose file is lost.  (Crash
# recovery doesn't check invalid pages, for md.c's either, and goes on.)
{
	my $node = PostgreSQL::Test::Cluster->new('lost_root');
	$node->init(allows_streaming => 1);
	$node->append_conf(
		'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
full_page_writes = off
});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	$sql->(q{CREATE TABLE t (a int, b text);
		INSERT INTO t SELECT g, repeat('t', 200) FROM generate_series(1, 1000) g});
	$node->backup('base');
	my $standby = PostgreSQL::Test::Cluster->new('lost_root_standby');
	$standby->init_from_backup($node, 'base', has_streaming => 1);
	$standby->start;
	$sql->('CHECKPOINT');
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	$standby->safe_psql('postgres', 'CHECKPOINT');
	$sql->(q{UPDATE t SET b = 'x' WHERE a = 7});
	$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
	my $tpath = $standby->data_dir . '/'
	  . $sql->(q{SELECT pg_relation_filepath('t')}) . '_bmap';
	$standby->stop('immediate');
	truncate($tpath, 8192) or die "truncate $tpath: $!";
	my $log_from = -s $standby->logfile;
	# (started by pg_ctl, not waited for: recovery may PANIC, and the
	# postmaster exit, before a start would read its pid file)
	PostgreSQL::Test::Utils::system_log('pg_ctl', '--no-wait',
		'--pgdata' => $standby->data_dir,
		'--log' => $standby->logfile, 'start');
	ok($standby->wait_for_log(qr/PANIC:  WAL contains references to invalid pages/,
			$log_from),
		'a standby whose map lost its root page fails recovery, its page invalid');
	# (the postmaster exits once its startup process PANICs)
	my $pidfile = $standby->data_dir . '/postmaster.pid';
	for (my $i = 0; -e $pidfile && $i < 10 * $PostgreSQL::Test::Utils::timeout_default; $i++)
	{
		usleep(100_000);
	}
	ok(!-e $pidfile, 'and its postmaster exits');
	PostgreSQL::Test::Utils::system_log('pg_ctl', '--pgdata' => $standby->data_dir,
		'--mode' => 'immediate', 'stop') if -e $pidfile;
	$node->stop;
}

done_testing();
