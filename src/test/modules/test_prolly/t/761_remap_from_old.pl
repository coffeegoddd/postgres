# Copyright (c) 2026, PostgreSQL Global Development Group

# The first change of a heap page a root (or another map) holds copies it
# to a block of its own.  The copy's record carries no image of the page: the
# old block holds it durably, logged as an image when it was shared, and
# nothing writes it in place since, so redo copies it from there.  After
# a crash that replays such copies, and on a standby, the rows are as
# they were, every page checked against the primary's
# (wal_consistency_checking), and the root still gives its own.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('remap_from_old');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_compression = off
wal_consistency_checking = 'all'
prolly.restore_min_changes = 0
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('remap_from_old_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $contents = q{SELECT count(*), sum(v), sum(length(pad)) FROM app.t};
sql(q{CREATE EXTENSION pg_walinspect; CREATE EXTENSION amcheck;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	INSERT INTO app.t SELECT i, 0, repeat('x', 300) FROM generate_series(1, 4000) i;
	SELECT dolt_commit('-Am', 'base')});
my $at_base = sql($contents);
sql('CHECKPOINT');

# changes of heap pages the commit's roots hold: copies, logged without
# images (an index's pages are logged with theirs)
my $start = sql(q{SELECT pg_current_wal_insert_lsn()});
sql(q{UPDATE app.t SET v = 1 WHERE id % 20 = 0});
my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
# (images wal_consistency_checking adds for checking aren't applied)
my ($copies, $with_images) = split(/\|/, sql(qq{
	SELECT count(DISTINCT start_lsn),
		count(DISTINCT start_lsn) FILTER (WHERE 'APPLY' = ANY(block_fpi_info))
	FROM pg_get_wal_block_info('$start', '$end')
	WHERE resource_manager = 'BlockMap' AND record_type = 'REMAP'
		AND description LIKE '%/' || pg_relation_filenode('app.t') || ';%'}));
cmp_ok($copies, '>', 0, 'the update copied heap pages the roots hold');
is($with_images, '0', 'and logged no image of them');
my $dirty = sql($contents);

# a crash: recovery from the checkpoint before replays the copies
$node->stop('immediate');
$node->start;
is(sql($contents), $dirty, 'after a crash, the rows as they were');
is(sql(q{SELECT bool_and(bt_index_check(indexrelid, true) IS NOT NULL)
	FROM pg_index WHERE indrelid = 'app.t'::regclass}), 't', 'the index valid');

# the standby replayed them too
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $contents), $dirty, 'the standby agrees');

# the roots still give the commit's rows
sql(q{SELECT dolt_reset('--hard')});
is(sql($contents), $at_base, 'a reset gives the commit\'s rows back');

$standby->stop;
$node->stop;
done_testing();
