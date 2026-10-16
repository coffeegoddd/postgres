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
$node->wait_for_replay_catchup($standby);
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
my $after = sql($listed);
like($after, qr/^copy \d+ \d+$/, 'what changed before the second fold is let go');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $listed), $after, 'on the standby too');
$node->stop('immediate');
$node->start;
is(sql($listed), $after, 'and after a crash');

$standby->stop;
$node->stop;
done_testing();
