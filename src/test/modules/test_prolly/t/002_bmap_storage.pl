# Copyright (c) 2026, PostgreSQL Global Development Group

# Block-mapped storage: heap tables, their indexes and their TOAST tables
# keep their pages in their database's page store, mapped by a WAL-logged
# block map.  They must survive crashes, reach standbys, be reused after a
# drop, pass base backups and pg_checksums, and behave as md.c's tables do
# under rewrites, persistence changes and tablespace drops.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('bmap_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
});
$primary->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

my $pgdata = $primary->data_dir;
my $dboid = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});

# Is a relation block-mapped?  It has a map fork.
sub block_mapped
{
	my ($node, $rel) = @_;
	my $path = $node->safe_psql('postgres', "SELECT pg_relation_filepath('$rel')");
	return -f $node->data_dir . "/${path}_bmap" ? 1 : 0;
}

my $digest = q{
	SELECT count(*) || ':' || md5(string_agg(id || pad || coalesce(length(t), 0), ',' ORDER BY id))
	FROM big};

# A table of more than a thousand blocks, so its map grows a level and
# takes new map pages while the table's extension lock is held; TOAST
# values; btree and hash indexes.
sql(q{
	CREATE TABLE big (id int PRIMARY KEY, pad text, t text);
	INSERT INTO big
		SELECT g, repeat('p', 80) || g,
			   CASE WHEN g % 500 = 0
					THEN (SELECT string_agg(md5(g::text || k), '') FROM generate_series(1, 80) k)
			   END
		FROM generate_series(1, 90000) g;
});
cmp_ok(sql(q{SELECT pg_relation_size('big') / 8192}), '>', 1019,
	'the table outgrows one map leaf');
is(block_mapped($primary, 'big'), 1, 'the table is block-mapped');
is(block_mapped($primary, 'big_pkey'), 1, 'its index is block-mapped');
is(block_mapped($primary,
		sql(q{SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'big'})),
	1, 'its TOAST table is block-mapped');
is(block_mapped($primary, 'pg_class'), 0, 'catalogs are not');

# The checkpointer writes block-mapped pages, reading the map to do so.
sql('CHECKPOINT');

# Changes after the checkpoint: index builds that log pages before writing
# them (btree, and hash, which allocates its buckets ahead), updates,
# deletes, VACUUM, a truncated and refilled table, a dropped table.
sql(q{
	CREATE INDEX big_pad ON big (pad);
	CREATE INDEX big_hash ON big USING hash (id);
	UPDATE big SET pad = 'u' || id WHERE id % 7 = 0;
	DELETE FROM big WHERE id % 11 = 0;
	VACUUM big;
	CREATE TABLE refill (id int);
	INSERT INTO refill SELECT generate_series(1, 5000);
	TRUNCATE refill;
	INSERT INTO refill SELECT generate_series(1, 300);
	CREATE TABLE gone AS SELECT generate_series(1, 10000) AS id;
	DROP TABLE gone;
});
my $before = sql($digest);

$primary->stop('immediate');
$primary->start;

# After a restart the map's pages aren't cached, so reading the table
# reads the map before each asynchronous read is started.
is(sql($digest), $before, 'the table is intact after a crash');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM big WHERE id BETWEEN 100 AND 200}),
	sql(q{SET enable_indexscan = off; SET enable_bitmapscan = off;
		  SELECT count(*) FROM big WHERE id BETWEEN 100 AND 200}),
	'its index agrees with it');
is(sql(q{SET enable_seqscan = off; SELECT pad FROM big WHERE id = 700}), 'u700',
	'the hash index built before the crash finds rows');
is(sql(q{SELECT count(*) || ',' || max(id) FROM refill}), '300,300',
	'a truncated and refilled table is intact');
is(sql(q{SELECT to_regclass('gone')}), '', 'a dropped table stays dropped');

# A crash loses an uncommitted first insert's WAL, but the map page it
# added to the map fork stays, all zeroes; the next insert uses it.
sql('CREATE TABLE lost (id int)');
my $session = $primary->background_psql('postgres', on_error_stop => 0);
$session->query_safe('BEGIN; INSERT INTO lost SELECT generate_series(1, 1000);');
$primary->stop('immediate');
$session->quit;
$primary->start;
sql('INSERT INTO lost SELECT generate_series(1, 1000)');
is(sql('SELECT count(*) FROM lost'), '1000',
	'a map page left by a lost extension is used again');

# Freed blocks are reused: a dropped table's blocks hold a new table of the
# same size without growing the page store.
my $store_size = sub {
	sql(qq{SELECT (pg_stat_file('base/$dboid/9421')).size});
};
sql(q{CREATE TABLE first AS SELECT generate_series(1, 100000) AS id});
sql('DROP TABLE first');
# (its storage, queued by the drop, let go of)
sql(q{CREATE EXTENSION test_prolly});
sql(q{SELECT * FROM test_bmap_release_drain()});
sql('CHECKPOINT');
my $size = $store_size->();
sql(q{CREATE TABLE second AS SELECT generate_series(1, 100000) AS id});
is($store_size->(), $size, 'a new table reuses a dropped table\'s blocks');
is(sql(q{SELECT count(*) FROM second}), '100000', 'and reads back');

# A rewrite keeps a table block-mapped, and a rewrite of a shared catalog
# keeps it out of block-mapped storage (connecting needs it).
sql('VACUUM FULL big');
is(block_mapped($primary, 'big'), 1, 'VACUUM FULL keeps a table block-mapped');
sql('VACUUM FULL pg_database');
is(sql('SELECT 1'), '1', 'a shared catalog rewritten under the setting still opens');

# Changing persistence moves a table out of block-mapped storage and back.
sql(q{CREATE TABLE persist (id int PRIMARY KEY); INSERT INTO persist VALUES (1)});
sql('ALTER TABLE persist SET UNLOGGED');
is(block_mapped($primary, 'persist'), 0, 'an unlogged table is not block-mapped');
sql('ALTER TABLE persist SET LOGGED');
is(sql('SELECT count(*) FROM persist'), '1', 'and comes back logged');

# A tablespace whose block-mapped tables are gone can be dropped.
my $tsdir = PostgreSQL::Test::Utils::tempdir();
sql("CREATE TABLESPACE bmap_ts LOCATION '$tsdir'");
sql(q{CREATE TABLE in_ts (id int) TABLESPACE bmap_ts; INSERT INTO in_ts VALUES (1)});
is(block_mapped($primary, 'in_ts'), 1, 'a table in a tablespace is block-mapped');
sql('DROP TABLE in_ts');
sql('DROP TABLESPACE bmap_ts');
is(sql(q{SELECT count(*) FROM pg_tablespace WHERE spcname = 'bmap_ts'}), '0',
	'its tablespace can be dropped');

# A base backup verifies checksums of every file but the page store, and a
# standby made from it follows the primary's map.
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('bmap_standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;
sql(q{
	INSERT INTO big SELECT g, 'standby' || g, NULL FROM generate_series(90001, 95000) g;
	DELETE FROM big WHERE id % 13 = 0;
	CREATE TABLE later AS SELECT generate_series(1, 20000) AS id;
	TRUNCATE second;
});
$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), sql($digest),
	'the standby reads the table as the primary does');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM later'), '20000',
	'and a table made after the backup');

# The standby, stopped, passes pg_checksums; and the primary keeps going
# after its standby is promoted.
$standby->stop;
command_ok([ 'pg_checksums', '--check', '--pgdata' => $standby->data_dir ],
	'pg_checksums passes on block-mapped storage');
$standby->start;
$standby->promote;
$standby->safe_psql('postgres', q{INSERT INTO big VALUES (100001, 'promoted', NULL)});
is($standby->safe_psql('postgres', 'SELECT pad FROM big WHERE id = 100001'), 'promoted',
	'the promoted standby writes block-mapped tables');

$standby->stop;
$primary->stop;

# At wal_level minimal, a table made in its transaction syncs its pages at
# commit instead of logging them.  Replaying an earlier table's allocation
# zeroes that table's blocks again; if the new table had reused them after
# the earlier one was dropped, nothing would restore its rows.  So such a
# table never takes freed blocks.
my $minimal = PostgreSQL::Test::Cluster->new('bmap_minimal');
$minimal->init;
$minimal->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
wal_level = minimal
max_wal_senders = 0
wal_skip_threshold = 0
autovacuum = off
});
$minimal->start;
$minimal->safe_psql('postgres', 'CREATE TABLE old_t (id int)');
$minimal->safe_psql('postgres', 'CHECKPOINT');
$minimal->safe_psql('postgres', 'INSERT INTO old_t SELECT generate_series(1, 10000)');
$minimal->safe_psql('postgres', 'DROP TABLE old_t');
$minimal->safe_psql('postgres',
	'CREATE TABLE new_t AS SELECT generate_series(20001, 30000) AS id');
# no scan before the crash: a hint bit's full-page image would hide the loss
$minimal->stop('immediate');
$minimal->start;
is($minimal->safe_psql('postgres', 'SELECT count(*) FROM new_t'), '10000',
	'a table whose pages skipped WAL keeps them through redo of earlier allocations');
$minimal->stop;

done_testing();
