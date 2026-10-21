# Copyright (c) 2026, PostgreSQL Global Development Group

# A conjoin WAL-logs the chunks it copies from the table files it
# replaces as where they come from, not as their bytes (#564): a standby
# replays it from its own files, byte for byte, and crash recovery after
# it, from a checkpoint before, finds the files it copied from removed
# and the file it wrote already whole.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('conjoin_copies');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE cj');
$node->safe_psql('cj', 'CREATE EXTENSION test_prolly');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('conjoin_copies_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('cj', $_[0]); }

my $files = q{SELECT string_agg(f || ' ' || md5(pg_read_binary_file(d || '/' || f)), E'\n' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'cj') db,
		pg_ls_dir(d) f
	WHERE (f LIKE 'prolly\_journal%' OR f LIKE 'prolly\_table\_%'
		OR f = 'prolly_manifest')};
my $count = q{SELECT count(*) FROM test_chunk_table_files()};

# Table files of 32 kB chunks of noise (LZ4 can't shrink them), each
# chunk's md5 listed (in a heap table, so the standby has the list).
sql(q{CREATE TABLE chunks (hash text PRIMARY KEY, md5 text) USING heap});
sub table_files
{
	my ($name, $nfiles) = @_;
	for my $f (1 .. $nfiles)
	{
		my $chunk = "test_noise('$name $f ' || g, 32768)";
		sql(qq{SELECT test_chunk_table_file(array_agg($chunk))
			FROM generate_series(1, 30) g});
		sql(qq{INSERT INTO chunks SELECT test_content_hash($chunk), md5($chunk)
			FROM generate_series(1, 30) g});
	}
}
my $reads = q{SELECT count(*) FILTER (WHERE md5(test_chunk_get(hash)) = md5)
	|| ' of ' || count(*) FROM chunks};
sub all_read { my $c = sql('SELECT count(*) FROM chunks'); return "$c of $c"; }

# A conjoin's WAL, and what it copied: (WAL bytes, bytes copied).
my $copied_stat = q{SELECT value FROM pg_stat_prolly WHERE name = 'conjoin_bytes_copied'};
sub conjoin
{
	my ($settings) = @_;
	my $before = $node->lsn('insert');
	my $copied_before = sql($copied_stat);
	my $out = $node->safe_psql('cj', qq{$settings
		SELECT test_chunk_conjoin();
		SELECT pg_stat_force_next_flush();
		SELECT ($copied_stat) - $copied_before});
	my $copied = (split(/\n/, $out))[-1];
	my $wal = $node->safe_psql('cj',
		"SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$before')");
	return ($wal, $copied);
}

table_files('first', 4);
is(sql($count), '4', 'four table files');
my ($wal, $copied) = conjoin('');
is(sql($count), '1', 'conjoined as one');
cmp_ok($copied, '>', 120 * 32768, 'their chunks copied');
cmp_ok($wal, '<', $copied / 50,
	"the conjoin's WAL is a small part of what it copied ($wal of $copied bytes)");
note "conjoin: $wal bytes of WAL for $copied bytes copied";
is(sql($reads), all_read(), 'the chunks read');

table_files('second', 3);
($wal, $copied) = conjoin('SET prolly.log_copied_bytes = on;');
cmp_ok($wal, '>', $copied,
	"with prolly.log_copied_bytes, the bytes are logged ($wal of $copied bytes)");
note "the same, its bytes logged: $wal bytes of WAL for $copied bytes copied";

$node->wait_for_catchup($standby);
is($standby->safe_psql('cj', $files), sql($files),
	"the standby's store files are the primary's, byte for byte");
is($standby->safe_psql('cj', $reads), all_read(), 'and it reads the chunks');

# Crash recovery from a checkpoint before a conjoin: the files it copied
# from are gone, the file it wrote whole.
table_files('third', 3);
sql('CHECKPOINT');
conjoin('');
my $after = sql($files);
$node->stop('immediate');
$node->start;
is(sql($reads), all_read(), 'crash recovery replays a conjoin whose sources are gone');
is(sql($files), $after, 'and leaves the files as they were');

# A standby restarted from before the conjoin likewise.
$node->wait_for_catchup($standby);
$standby->stop('immediate');
$standby->start;
is($standby->safe_psql('cj', $reads), all_read(), 'so does a standby restarted');
$node->wait_for_catchup($standby);
is($standby->safe_psql('cj', $files), sql($files), 'its files still the primary\'s');

done_testing();
