# Copyright (c) 2026, PostgreSQL Global Development Group

# At wal_level minimal, blocks a page-store shrink cut off are handed out
# again from the lowered high-water mark: to a relation whose writes skip
# WAL (always given blocks from the mark, synced at commit), or to pages a
# share or snapshot syncs rather than logs.  Crash recovery replays the
# shrink before the extensions that took the blocks again; truncating the
# store there would cut pages nothing later in the replay writes.  Each
# case shrinks the store after a checkpoint, writes into the blocks it cut,
# crashes, and reads every page back.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('minimal_shrink_crash');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_level = minimal
max_wal_senders = 0
autovacuum = off
autovacuum_naptime = 1s
checkpoint_timeout = '1h'
prolly.restore_min_changes = 100000000
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub crash { $node->stop('immediate'); $node->start; }

sql(q{CREATE EXTENSION amcheck; CREATE SCHEMA app VERSIONED});
my $store = $node->data_dir . '/base/' .
  sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'}) . '/9421';

# Blocks for a shrink to cut: a large table's, dropped and committed.
sub junk
{
	sql(q{CREATE TABLE app.junk (id int PRIMARY KEY, v text);
		INSERT INTO app.junk SELECT g, repeat('x', 500) FROM generate_series(1, 20000) g;
		SELECT dolt_commit('-Am', 'junk')});
	sql(q{DROP TABLE app.junk; SELECT dolt_commit('-Am', 'no junk')});
}

# Every page of table $t, its index, its change log and the log's index,
# read: the rows counted by a scan of each, each btree checked.
sub check
{
	my ($t, $what) = @_;
	my ($ret, $out, $err) = $node->psql('postgres', qq{
		SELECT count(*), sum(v) FROM app.$t;
		SET enable_seqscan = off;
		SELECT count(*) FROM app.$t WHERE id > 0;
		RESET enable_seqscan;
		SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
			FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
			WHERE i.indrelid = 'app.$t'::regclass
			   OR c.relname = 'pg_prolly_log_' || 'app.$t'::regclass::oid || '_index';
	});
	is($ret, 0, "$what: every page reads") or diag($err);
	return $out;
}

# A table made and loaded in one transaction, its writes skipping WAL and
# synced at commit, in blocks an explicit sweep's shrink cut.
junk();
sql(q{CHECKPOINT});
cmp_ok(sql(q{SELECT truncated FROM prolly_page_sweep()}), '>', 0,
	'the sweep shrinks the page store');
sql(q{SET wal_skip_threshold = 0; BEGIN;
	CREATE TABLE app.w (id int PRIMARY KEY, v int NOT NULL);
	INSERT INTO app.w SELECT g, 1 FROM generate_series(1, 3000) g;
	COMMIT});
crash();
is(check('w', 'a WAL-skipped table after a shrink and a crash'),
	"3000|3000\n3000\nt", 'its rows');

# The same, with autovacuum's sweep doing the shrink.
junk();
sql(q{CHECKPOINT});
my $size = -s $store;
sql(q{ALTER SYSTEM SET autovacuum = on; SELECT pg_reload_conf()});
my $shrunk = 0;
foreach (1 .. $PostgreSQL::Test::Utils::timeout_default * 10)
{
	if (-s $store < $size) { $shrunk = 1; last; }
	usleep(100_000);
}
sql(q{ALTER SYSTEM RESET autovacuum; SELECT pg_reload_conf()});
ok($shrunk, 'autovacuum\'s sweep shrinks the page store');
sql(q{SET wal_skip_threshold = 0; BEGIN;
	CREATE TABLE app.a (id int PRIMARY KEY, v int NOT NULL);
	INSERT INTO app.a SELECT g, 2 FROM generate_series(1, 3000) g;
	COMMIT});
crash();
is(check('a', 'a WAL-skipped table after autovacuum\'s shrink and a crash'),
	"3000|6000\n3000\nt", 'its rows');

# Pages a snapshot syncs, in blocks a shrink cut: the table's writes are
# logged, but its storage is replaced by a hard reset, so recovery skips
# the records that made the kept commit's pages.
sql(q{CREATE TABLE app.s (id int PRIMARY KEY, v int NOT NULL);
	SELECT dolt_commit('-Am', 's0')});
junk();
sql(q{CHECKPOINT});
cmp_ok(sql(q{SELECT truncated FROM prolly_page_sweep()}), '>', 0,
	'the sweep shrinks the page store again');
sql(q{INSERT INTO app.s SELECT g, 3 FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-am', 's1');
	UPDATE app.s SET v = 4 WHERE id % 7 = 0; SELECT dolt_reset('--hard');
	UPDATE app.s SET v = 5 WHERE id % 13 = 0});
crash();
sql(q{SELECT dolt_reset('--hard')});
is(check('s', 'a kept commit after a shrink and a crash'),
	"3000|9000\n3000\nt", 'the commit\'s rows');

$node->stop;
done_testing();
