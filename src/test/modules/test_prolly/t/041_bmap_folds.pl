# Copyright (c) 2026, PostgreSQL Global Development Group

# Folds of block-mapped relations survive crashes and reach standbys: the
# changes list, and the blocks it holds as the pages were at the fold.
# Recovery writes a listed block from its record, so a write of it that a
# crash lost (here, zeroes written over it) comes back.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('folds');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION pageinspect');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('folds_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA f VERSIONED;
	CREATE TABLE f.t (a int8 PRIMARY KEY, b int8);
	INSERT INTO f.t SELECT g, g FROM generate_series(1, 3000) g});
my $rel = sql(q{SELECT pg_relation_filenode('f.t')});
my $items = q{SELECT md5(string_agg(lp || ':' || coalesce(t_xmin::text, '') || ':'
	|| coalesce(t_xmax::text, '') || ':' || coalesce(t_data::text, ''), ',' ORDER BY lp))
	FROM heap_page_items(%s)};
my $before = sql(sprintf($items, q{get_raw_page('f.t', 0)}));
sql('CHECKPOINT');

my ($lineage, $epoch) = split /\|/, sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($rel)});
sql(q{UPDATE f.t SET b = -b WHERE a IN (1, 2, 1500)});
my $listed = qq{SELECT string_agg(kind || ' ' || blkno || ' ' || coalesce(old_block::text, '-'),
	', ' ORDER BY blkno) FROM test_bmap_fold_changes($rel, $lineage, $epoch)};
my $changes = sql($listed);
my ($old0) = $changes =~ /copy 0 (\d+)/;
ok(defined $old0, 'the first page changed is listed');
is(sql(sprintf($items, "test_bmap_read_old($rel, 0, $old0)")), $before,
	'its block holds the page as of the fold');

# The standby has the same list and the same kept page.
# (to the WAL inserted: the changes come from transactions without an
# xid, whose commits don't flush it)
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $listed), $changes, 'a standby has the list');
is($standby->safe_psql('postgres', sprintf($items, "test_bmap_read_old($rel, 0, $old0)")),
	$before, 'and the kept page');

# A crash that lost the kept page's write: recovery writes it again.
my $dboid = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
$node->stop('immediate');
my $store = $node->data_dir . "/base/$dboid/9421";
open(my $fh, '+<', $store) or die "could not open $store: $!";
binmode $fh;
sysseek($fh, $old0 * 8192, 0) or die "seek: $!";
syswrite($fh, "\0" x 8192) == 8192 or die "write: $!";
close $fh;
$node->start;
is(sql(sprintf($items, "test_bmap_read_old($rel, 0, $old0)")), $before,
	'recovery writes the kept page again');
is(sql($listed), $changes, 'and the list survives');
is(sql('SELECT count(*), sum(b) FROM f.t'), '3000|4498494', 'with the rows');

# Truncations and prunes of the list replay too.
sql(q{DELETE FROM f.t WHERE ctid >= '(14,0)'; VACUUM f.t});
my ($epoch2) = sql(qq{SELECT epoch FROM test_bmap_fold_begin($rel)});
sql(q{UPDATE f.t SET b = 0 WHERE a = 3});
sql(qq{SELECT test_bmap_fold_prune($rel, $lineage, $epoch2)});
my $listed2 = qq{SELECT string_agg(kind || ' ' || blkno || ' ' || coalesce(old_block::text, '-'),
	', ' ORDER BY blkno) FROM test_bmap_fold_changes($rel, $lineage, $epoch2)};
my $after = sql($listed2);
like($after, qr/^copy \d+ \d+$/, 'what changed before the second fold is let go');
is(sql($listed), '', 'and what changed since the first is not known');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $listed2), $after, 'on the standby too');
$node->stop('immediate');
$node->start;
is(sql($listed2), $after, 'and after a crash');

# A standby doesn't prune: that would be a change during recovery.
my ($ret, $out, $err) = $standby->psql('postgres',
	"SELECT test_bmap_fold_prune($rel, $lineage, " . ($epoch2 + 1) . ")");
like($err, qr/cannot prune a relation's changes during recovery/, 'a standby refuses to prune');
is($standby->safe_psql('postgres', 'SELECT 1'), '1', 'and stays up');

# A listed block another relation shares keeps what it has in redo: the
# change's image has hints only this relation's buffer had, which the
# primary never writes there.
sql(q{CREATE TABLE f.h (a int8 PRIMARY KEY, b int8);
	INSERT INTO f.h SELECT g, g FROM generate_series(1, 100) g});
my $hrel = sql(q{SELECT pg_relation_filenode('f.h')});
sql('CHECKPOINT');
my $hcopy = sql(qq{SELECT test_bmap_share($hrel)});
sql('SELECT count(*) FROM f.h');
my $hints = q{SELECT count(*) FROM heap_page_items(%s) WHERE t_infomask & 256 <> 0};
cmp_ok(sql(sprintf($hints, q{get_raw_page('f.h', 0)})), '>', 0,
	'a scan hints the page in its buffer');
my ($hl, $he) = split /\|/, sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($hrel)});
sql(q{UPDATE f.h SET b = 0 WHERE a = 1});
my ($hold) = sql(qq{SELECT old_block FROM test_bmap_fold_changes($hrel, $hl, $he) WHERE blkno = 0});
my $kept_hints = sprintf($hints, "test_bmap_read_old($hrel, 0, $hold)");
is(sql($kept_hints), '0', 'the shared block it leaves has none');
$node->stop('immediate');
$node->start;
is(sql($kept_hints), '0', 'nor after recovery');
sql(qq{SELECT test_bmap_unlink($hcopy)});

# Copies, prunes and truncations, at their awkward moments.
SKIP:
{
	skip 'injection points not supported by this build', 6
	  if $ENV{enable_injection_points} ne 'yes';
	sql('CREATE EXTENSION injection_points');

	# a table with copies listed before its second fold
	my $listed_table = sub {
		my ($name) = @_;
		sql(qq{CREATE TABLE f.$name (a int8 PRIMARY KEY, b int8);
			INSERT INTO f.$name SELECT g, g FROM generate_series(1, 3000) g});
		my $r = sql(qq{SELECT pg_relation_filenode('f.$name')});
		my ($l, $e1) = split /\|/, sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($r)});
		sql(qq{UPDATE f.$name SET b = -b WHERE a IN (1, 1500, 2900)});
		my ($e2) = split /\|/, sql(qq{SELECT epoch FROM test_bmap_fold_begin($r)});
		my ($n, $nb) = split /\|/, sql(qq{SELECT count(*), count(DISTINCT old_block)
			FROM test_bmap_fold_changes($r, $l, $e1)});
		return ($r, $l, $e1, $e2, $n, $nb);
	};
	my $waiting = sub {
		my ($event, $what) = @_;
		$node->poll_query_until('postgres', qq{SELECT count(*) > 0 FROM pg_stat_activity
			WHERE wait_event = '$event'}) or die "$what did not wait";
	};
	my $prune_waits = q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event_type = 'Buffer' AND query LIKE '%test_bmap_fold_prune%'};

	# A prune waits for a copy being made, which counts what the list holds.
	my ($prel, $pl, $pe1, $pe2, $pn, $pnb) = $listed_table->('p');
	my $s1 = $node->background_psql('postgres');
	$s1->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-share-changes-counted', 'wait')});
	$s1->query_until(qr/sharing/, qq{
		\\echo sharing
		SELECT 'copy ' || test_bmap_share($prel);
		\\echo shared
	});
	$waiting->('bmap-share-changes-counted', 'the copy');
	my $s2 = $node->background_psql('postgres');
	$s2->query_until(qr/pruning/, qq{
		\\echo pruning
		SELECT test_bmap_fold_prune($prel, $pl, $pe2);
		\\echo pruned
	});
	ok($node->poll_query_until('postgres', $prune_waits), 'a prune waits for a copy being made');
	sql(q{SELECT injection_points_wakeup('bmap-share-changes-counted')});
	my ($pcopy) = $s1->query_until(qr/shared/, '') =~ /copy (\d+)/;
	$s2->query_until(qr/pruned/, '');
	$s1->quit;
	$s2->quit;
	my $free = sql('SELECT test_bmap_free_blocks()');
	sql(qq{SELECT test_bmap_unlink($pcopy)});
	is(sql('SELECT test_bmap_free_blocks()') - $free, $pnb,
		'dropping the copy frees the blocks the prune let go of');

	# A crash once a copy's blocks are counted leaves its list holding
	# them, so dropping it gives them back.
	my ($crel, $cl, $ce1, $ce2, $cn, $cnb) = $listed_table->('c');
	my $lists = q{SELECT string_agg(f, ',') FROM
		(SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'postgres') db,
		pg_ls_dir(d) f WHERE f LIKE '%\_chg'};
	my %before = map { $_ => 1 } split /,/, sql($lists);
	$s1 = $node->background_psql('postgres');
	$s1->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-share-changes-counted', 'wait')});
	$s1->query_until(qr/sharing/, qq{
		\\echo sharing
		SELECT test_bmap_share($crel);
	});
	$waiting->('bmap-share-changes-counted', 'the copy');
	# (a commit flushes the copy's records, which have no transaction's)
	sql('SELECT txid_current()');
	$node->stop('immediate');
	eval { $s1->quit };
	$node->start;
	# (the newest list: replay makes again those of copies dropped since)
	my ($ccopy) = sort { $b <=> $a } map { /^(\d+)_chg$/ ? $1 : () }
	  grep { !$before{$_} } split /,/, sql($lists);
	is(sql(qq{SELECT count(*) FROM test_bmap_fold_changes($ccopy, $cl, $ce1)}), $cn,
		'the copy cut short lists what it counted');
	sql(qq{SELECT test_bmap_unlink($ccopy)});
	$free = sql('SELECT test_bmap_free_blocks()');
	sql(qq{SELECT test_bmap_fold_prune($crel, $cl, $ce2)});
	is(sql('SELECT test_bmap_free_blocks()') - $free, $cnb,
		'and dropping it gives them back');

	# A prune waits for a truncation prepared, which lists itself.
	my ($trel, $tl, $te1, $te2) = $listed_table->('tr');
	$s1 = $node->background_psql('postgres');
	$s1->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-pretruncated', 'wait')});
	$s1->query_until(qr/truncating/, qq{
		\\echo truncating
		SELECT test_bmap_truncate($trel, 5);
		\\echo truncated
	});
	$waiting->('bmap-pretruncated', 'the truncation');
	$s2 = $node->background_psql('postgres');
	$s2->query_until(qr/pruning/, qq{
		\\echo pruning
		SELECT test_bmap_fold_prune($trel, $tl, $te2);
		\\echo pruned
	});
	ok($node->poll_query_until('postgres', $prune_waits),
		'a prune waits for a truncation prepared');
	sql(q{SELECT injection_points_wakeup('bmap-pretruncated')});
	$s1->query_until(qr/truncated/, '');
	$s2->query_until(qr/pruned/, '');
	$s1->quit;
	$s2->quit;
	is(sql(qq{SELECT string_agg(kind || ' ' || blkno, ',') FROM test_bmap_fold_changes($trel, $tl, $te2)}),
		'truncate 5', 'which is listed');
	sql(q{DROP TABLE f.tr});
}

$standby->stop;
$node->stop;
done_testing();
