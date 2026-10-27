# Copyright (c) 2026, PostgreSQL Global Development Group

# Captured roots' records at volume: a thousand roots captured, some let
# go of, some captured by transactions that roll back or that a crash
# ends.  Recovery and a standby replay them, every block map page checked
# against the primary's (wal_consistency_checking); the roots held, their
# pages, and the marks of the pages of slots naming a transaction come
# out as the primary had them, so the settling after reads only those.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root_records_redo');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_consistency_checking = 'BlockMap'
debug_block_mapped_heap = on
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('root_records_redo_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "wal_consistency_checking = 'BlockMap'");
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE TABLE big (a int, b text) WITH (fillfactor = 10, autovacuum_enabled = off);
	INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 6000) g;
	CREATE TABLE roots (n int PRIMARY KEY, id int8, rows text)});
sql('CHECKPOINT');

# a thousand roots, each after a row changed, the rows each holds noted
# for every hundredth
sql(q{DO $$
	BEGIN
		FOR i IN 1..1000 LOOP
			UPDATE big SET b = 'x' || i WHERE a = (i * 7919) % 6000 + 1;
			INSERT INTO roots VALUES (i, test_bmap_root_capture(pg_relation_filenode('big')),
				CASE WHEN i % 100 = 0 THEN test_bmap_rows_digest(pg_relation_filenode('big')) END);
			COMMIT;
		END LOOP;
	END $$});
# four hundred let go of, half of them drained
sql(q{SELECT count(test_bmap_root_release(id)) FROM roots WHERE n % 5 IN (1, 2)});
sql(q{SELECT test_bmap_release_drain(500)});
# captures rolled back, and one under way as the crash comes
sql(q{BEGIN; SELECT test_bmap_root_capture(pg_relation_filenode('big')); ROLLBACK});
my $open = $node->background_psql('postgres');
$open->query_safe(q{BEGIN; SELECT test_bmap_root_capture(pg_relation_filenode('big'))});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

my $held = q{SELECT held FROM test_bmap_roots()};
# (in recovery no transaction is judged: a slot not let go of holds, the
# rolled back capture's and the one under way's too, until reclaimed)
is($standby->safe_psql('postgres', $held), '602',
	'the standby holds the roots held, and the captures not yet reclaimed');

$node->stop('immediate');
$open->{run}->kill_kill;
$node->start;
my $log = slurp_file($node->logfile);
my ($redo) = $log =~ /redo done at .*elapsed: ([0-9.]+) s/;
note("crash recovery of the roots' records: $redo s");

# the capture the crash ended holds nothing once reclaimed; the rolled
# back one either; VACUUM settles every slot
sql(q{VACUUM roots});
is(sql($held), '600', 'after a crash, the roots held, none of the aborted');
is(sql(q{SELECT test_bmap_roots_naming()}), '0', 'every slot settled');
my $pages_read = q{SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'bmap_roots_pages_read'), 0)};
my $before = sql($pages_read);
sql(q{VACUUM roots});
is(sql($pages_read) - $before, 0, 'settled, a VACUUM reads no page of slots');

# each noted root (none let go of) still has the rows it was captured with; a sweep frees
# what the roots let go of alone, nothing they hold
sql(q{SELECT test_bmap_release_drain(100000000)});
sql(q{SELECT * FROM prolly_page_sweep()});
is(sql(q{SELECT count(*) FROM roots WHERE rows IS NOT NULL AND n % 5 NOT IN (1, 2) AND
	test_bmap_rows_digest(test_bmap_share_root(id)) = rows}), '10',
	'the roots kept hold their rows, after the crash and a sweep');

# the standby: the same roots, the same marks settled
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $held), '600', 'the standby holds the same roots');
is($standby->safe_psql('postgres', q{SELECT test_bmap_roots_naming()}), '0',
	'and its slots are settled too');
$standby->promote;
is($standby->safe_psql('postgres', q{SELECT count(*) FROM roots WHERE rows IS NOT NULL AND
	n % 5 NOT IN (1, 2) AND test_bmap_rows_digest(test_bmap_share_root(id)) = rows}), '10',
	'promoted, the standby\'s roots hold their rows');

$standby->stop;
$node->stop;
done_testing();
