# Copyright (c) 2026, PostgreSQL Global Development Group

# Hints on block-mapped pages, with data checksums on (and, as a variant,
# off with wal_log_hints on).  A hint set on a page another map shares
# doesn't dirty it: a tuple's hint bits, or an index entry marked dead in
# one relation only.  The shared block is never written in place, so no
# torn write can reach it, and each holder reads it as it was synced when
# it came to be shared.  A hint set on a relation's own page is protected
# as on heap: its first change after a checkpoint logs a full-page image,
# and a torn write of the page is repaired by redo.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub run_case
{
	my ($name, $initdb, $conf, $checksums) = @_;

	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(extra => $initdb);
	$node->append_conf(
		'postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
shared_buffers = 64MB
$conf
});
	$node->start;
	my $sql = sub { return $node->safe_psql('postgres', $_[0]); };
	$sql->(q{CREATE EXTENSION test_prolly; CREATE EXTENSION pg_buffercache});
	is($sql->('SHOW data_checksums'), $checksums ? 'on' : 'off',
		"$name: data checksums are " . ($checksums ? 'on' : 'off'));

	my $dboid = $sql->(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
	my $store = $node->data_dir . "/base/$dboid/9421";
	my $block_bytes = sub {
		my ($phys) = @_;
		open(my $fh, '<:raw', $store) or die "open $store: $!";
		seek($fh, $phys * 8192, 0) or die "seek: $!";
		my $n = read($fh, my $buf, 8192);
		close($fh);
		die "short read" unless $n == 8192;
		return $buf;
	};
	# every block a relation's map holds, as on disk
	my $all_bytes = sub {
		my ($relnumber) = @_;
		return join('', map { $block_bytes->($_) } split /,/,
			$sql->(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno)
				FROM test_bmap_map($relnumber)}));
	};
	my $dirty = sub {
		my ($relnumber) = @_;
		return $sql->(qq{SELECT count(*) FROM pg_buffercache
			WHERE relfilenode = $relnumber AND isdirty});
	};

	# A table shared before its rows' hints are set: reading it sets them,
	# on pages its copy holds too.
	$sql->(q{CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
		INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 2000) g});
	my $rel = $sql->(q{SELECT pg_relation_filenode('t')});
	$sql->('CHECKPOINT');
	my $copy = $sql->(qq{SELECT test_bmap_share($rel)});
	my $before = $all_bytes->($rel);
	$sql->(q{SELECT count(*) FROM t});
	is($dirty->($rel), '0', "$name: hint bits set on shared pages dirty none of them");
	$sql->('CHECKPOINT');
	is($all_bytes->($rel) eq $before ? 'same' : 'changed', 'same',
		"$name: and the shared blocks are not written in place");
	is($sql->(qq{SELECT test_bmap_rows_digest($copy)}),
		$sql->(qq{SELECT test_bmap_rows_digest($rel)}),
		"$name: both read the same rows");

	# An index shared, then an entry of it marked dead in the source alone
	# (its row deleted there, and found dead by an index scan): the copy's
	# page isn't changed, its entry still live.
	$sql->(q{CREATE TABLE ti (a int PRIMARY KEY, b text);
		INSERT INTO ti SELECT g, repeat('i', 50) FROM generate_series(1, 2000) g});
	my $irel = $sql->(q{SELECT pg_relation_filenode('ti_pkey')});
	$sql->('CHECKPOINT');
	my $icopy = $sql->(qq{SELECT test_bmap_share($irel)});
	my $ibefore = $all_bytes->($icopy);
	$sql->(q{DELETE FROM ti WHERE a BETWEEN 500 AND 520});
	$sql->(q{SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT count(*) FROM ti WHERE a BETWEEN 400 AND 600;
		SELECT count(*) FROM ti WHERE a BETWEEN 400 AND 600});
	is($dirty->($irel), '0',
		"$name: an index entry marked dead on a shared page dirties it not");
	$sql->('CHECKPOINT');
	is($all_bytes->($icopy) eq $ibefore ? 'same' : 'changed', 'same',
		"$name: and the copy's pages keep the entry as it was");

	# A relation's own page: its hint's first change after a checkpoint
	# logs an image, and a torn write of the page is repaired by redo.
	$sql->(q{CREATE TABLE own (a int, b text) WITH (fillfactor = 50);
		INSERT INTO own SELECT g, repeat('o', 100) FROM generate_series(1, 2000) g});
	my $orel = $sql->(q{SELECT pg_relation_filenode('own')});
	$sql->('CHECKPOINT');
	my $digest = q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM own};
	# (the first read after the checkpoint sets the hints, each page's
	# image logged first: measured over that read, not a later one, whose
	# WAL is only what other hints it sets happen to log, a catalog page's)
	my $lsn = $sql->('SELECT pg_current_wal_insert_lsn()');
	my $want = $sql->($digest);
	cmp_ok($dirty->($orel), '>', 0, "$name: hints dirty the relation's own pages");
	cmp_ok($sql->(qq{SELECT pg_current_wal_insert_lsn() - '$lsn'::pg_lsn}), '>', 8192,
		"$name: and log their images");
	$sql->(qq{SELECT count(*) FROM pg_buffercache WHERE relfilenode = $orel
		AND (pg_buffercache_evict(bufferid)).buffer_evicted});
	my $ophys = $sql->(qq{SELECT phys FROM test_bmap_map($orel) WHERE blkno = 5});
	$node->stop('immediate');

	# a torn write, simulated: the block's second half overwritten with
	# garbage (bytes 0xAB)
	my $tear = sub {
		my ($phys) = @_;
		open(my $fh, '+<:raw', $store) or die "open $store: $!";
		seek($fh, $phys * 8192 + 4096, 0) or die "seek: $!";
		print $fh ("\xAB" x 4096) or die "write: $!";
		close($fh) or die "close: $!";
	};
	$tear->($ophys);
	$node->start;
	is($sql->($digest), $want,
		"$name: redo repairs a torn write of an own page from its hint's image");
	is($sql->(qq{SELECT test_bmap_rows_digest($copy)}),
		$sql->(qq{SELECT test_bmap_rows_digest($rel)}),
		"$name: and the shared pages read as before");

	# (a control: the same tear of a page with no image to repair it, its
	# hints written out by a checkpoint, is caught by its checksum)
	if ($checksums)
	{
		my $cphys = $sql->(qq{SELECT phys FROM test_bmap_map($orel) WHERE blkno = 7});
		$sql->('CHECKPOINT');
		$node->stop('immediate');
		$tear->($cphys);
		$node->start;
		my ($ret, $out, $err) = $node->psql('postgres', q{SELECT count(*) FROM own});
		like($err, qr/invalid page/,
			"$name: a tear redo has no image for is caught by the checksum");
	}
	$node->stop;
}

run_case('hint_checksums', ['--data-checksums'], '', 1);
run_case('hint_log_hints', ['--no-data-checksums'], 'wal_log_hints = on', 0);
done_testing();
