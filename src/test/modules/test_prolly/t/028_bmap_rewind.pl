# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_rewind accepts a cluster with block-mapped tables.  A standby is
# promoted, and the old primary goes on changing block-mapped tables (and
# sharing them) before it is stopped; pg_rewind then makes it follow the
# new primary, and it must read the new primary's rows: block-mapped
# storage has no file of its own for a relation's pages, whose changes the
# WAL names by logical block while they live in the page store's blocks.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;

my $primary = PostgreSQL::Test::Cluster->new('rewind_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
wal_log_hints = on
wal_keep_size = 512MB
});
$primary->start;

sub sql { return $_[0]->safe_psql('postgres', $_[1]); }

sql($primary, q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, 'before ' || g FROM generate_series(1, 20000) g;
	CREATE TABLE gone (a int);
	INSERT INTO gone SELECT generate_series(1, 1000);
	CHECKPOINT;
});
my $t = sql($primary, q{SELECT pg_relation_filenode('t')});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('rewind_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);

# The standby promoted, and both change the tables apart.
$standby->promote;
sql($standby, q{
	UPDATE t SET b = 'new primary ' || a WHERE a % 2 = 0;
	CREATE TABLE made_after (a int);
	INSERT INTO made_after SELECT generate_series(1, 500);
	CHECKPOINT;
});
my $rows_query = q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM t};
my $rows = sql($standby, $rows_query);

sql($primary, q{
	UPDATE t SET b = 'old primary ' || a WHERE a % 3 = 0;
	INSERT INTO t SELECT g, 'old primary' FROM generate_series(20001, 30000) g;
	DROP TABLE gone;
	CREATE TABLE diverged (a int);
	INSERT INTO diverged SELECT generate_series(1, 5000);
});
my $copy = sql($primary, qq{SELECT test_bmap_share($t)});
sql($primary, q{UPDATE t SET b = 'old primary again' WHERE a % 7 = 0});
$primary->stop;

# (pg_rewind copies the configuration from the source: the old primary's
# is kept aside and put back)
my $conf = $primary->data_dir . '/postgresql.conf';
my $kept = PostgreSQL::Test::Utils::tempdir . '/postgresql.conf';
copy($conf, $kept) or die "could not copy $conf: $!";
my $connstr = $standby->connstr('postgres');
command_ok(
	[ 'pg_rewind', '--debug',
	  '--source-server' => $connstr,
	  '--target-pgdata' => $primary->data_dir,
	  '--no-sync' ],
	'pg_rewind rewinds the old primary');

move($kept, $conf) or die "could not move $kept: $!";

# The old primary follows the new one.
$primary->enable_streaming($standby);
$primary->start;
$standby->wait_for_replay_catchup($primary);
is(sql($primary, $rows_query), $rows, 'the old primary reads the new primary\'s rows');
is(sql($primary, q{SELECT count(*) FROM made_after}), '500',
	'and the table made after the promotion');
is(sql($primary, q{SELECT count(*) FROM gone}), '1000',
	'and the table it dropped');
is(sql($primary, q{SELECT count(*) FROM pg_class WHERE relname = 'diverged'}), '0',
	'and not the table it made');

# Both go on: a change on the new primary reaches the old one.
sql($standby, q{UPDATE t SET b = 'after rewind' WHERE a = 1});
$standby->wait_for_replay_catchup($primary);
is(sql($primary, $rows_query), sql($standby, $rows_query),
	'a change after the rewind reaches it');

$primary->stop;
$standby->stop;
done_testing();
