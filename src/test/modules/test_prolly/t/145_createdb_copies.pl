# Copyright (c) 2026, PostgreSQL Global Development Group

# CREATE DATABASE WAL-logs the chunk store files it copies from its
# template as where they come from, not as their bytes, whatever its
# strategy (#564): a standby makes them from its own copy of the
# template, byte for byte; crash recovery replays the copies; a standby
# restarted from before a CREATE DATABASE whose template was dropped since
# finds the new files whole; and while an online backup runs, the bytes
# are logged (a template dropped during the backup may be missing from
# it).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('createdb_copies');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('createdb_copies_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'checkpoint_timeout = 1h');
$standby->start;

# A template whose store has journal chunks and a table file, of 32 kB
# chunks of noise (LZ4 can't shrink them), their md5s listed in a heap
# table the copies have too.
sub template
{
	my ($db) = @_;
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, q{CREATE EXTENSION test_prolly;
		CREATE TABLE chunks (hash text PRIMARY KEY, md5 text) USING heap});
	$node->safe_psql($db, q{INSERT INTO chunks
		SELECT test_chunk_put(test_noise('journal ' || g, 32768)),
			md5(test_noise('journal ' || g, 32768))
		FROM generate_series(1, 100) g});
	$node->safe_psql($db, q{SELECT test_chunk_table_file(array_agg(test_noise('file ' || g, 32768)))
		FROM generate_series(1, 50) g});
	$node->safe_psql($db, q{INSERT INTO chunks
		SELECT test_content_hash(test_noise('file ' || g, 32768)),
			md5(test_noise('file ' || g, 32768))
		FROM generate_series(1, 50) g});
}
my $reads = q{SELECT count(*) FILTER (WHERE md5(test_chunk_get(hash)) = md5)
	|| ' of ' || count(*) FROM chunks};
sub files
{
	my ($db) = @_;
	return qq{SELECT string_agg(f || ' ' || md5(pg_read_binary_file(d || '/' || f)), E'\n' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = '$db') db,
		pg_ls_dir(d) f
	WHERE (f LIKE 'prolly\\_journal%' OR f LIKE 'prolly\\_table\\_%'
		OR f = 'prolly_manifest')};
}
my $store_bytes = q{SELECT sum((pg_stat_file(d || '/' || f)).size)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = current_database()) db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_journal%' OR f LIKE 'prolly\_table\_%'};

# A CREATE DATABASE's WAL.
sub createdb
{
	my ($sql) = @_;
	my $before = $node->lsn('insert');
	$node->safe_psql('postgres', $sql);
	return $node->safe_psql('postgres',
		"SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$before')");
}

template('tpl');
my $bytes = $node->safe_psql('tpl', $store_bytes);
cmp_ok($bytes, '>', 150 * 32768, 'the template\'s store');

my $copies = createdb('CREATE DATABASE c1 TEMPLATE tpl');
my $logged = createdb(q{SET prolly.log_copied_bytes = on;
	CREATE DATABASE c2 TEMPLATE tpl});
cmp_ok($logged - $copies, '>', $bytes * 0.9,
	"copies log less than the bytes by about the store's size ($copies, against $logged, for $bytes bytes)");
note "CREATE DATABASE (WAL_LOG): $copies bytes of WAL; with the bytes logged, $logged; the store, $bytes bytes";
my $file_copy = createdb('CREATE DATABASE c3 TEMPLATE tpl STRATEGY FILE_COPY');
my $file_copy_logged = createdb(q{SET prolly.log_copied_bytes = on;
	CREATE DATABASE c4 TEMPLATE tpl STRATEGY FILE_COPY});
cmp_ok($file_copy_logged - $file_copy, '>', $bytes * 0.9,
	"so does the file-copy strategy ($file_copy, against $file_copy_logged)");
note "CREATE DATABASE (FILE_COPY): $file_copy bytes of WAL; with the bytes logged, $file_copy_logged";
for my $db ('c1', 'c2', 'c3', 'c4')
{
	is($node->safe_psql($db, $reads), '150 of 150', "$db reads its template's chunks");
	is($node->safe_psql($db, files($db)) =~ s/\n/ /gr ne '', 1, "$db has its files");
}
is($node->safe_psql('postgres', files('c1')) =~ s!base/\d+!!gr,
	$node->safe_psql('postgres', files('tpl')) =~ s!base/\d+!!gr,
	'a copy\'s files are its template\'s, byte for byte');

# A standby makes them from its own copy of the template.
$node->wait_for_catchup($standby);
for my $db ('c1', 'c3')
{
	is($standby->safe_psql('postgres', files($db)), $node->safe_psql('postgres', files($db)),
		"the standby's files of $db are the primary's");
	is($standby->safe_psql($db, $reads), '150 of 150', "and it reads them");
}

# Crash recovery replays the copies, from a checkpoint before them.
$node->safe_psql('postgres', 'CHECKPOINT');
createdb('CREATE DATABASE c5 TEMPLATE tpl');
my $c5 = $node->safe_psql('postgres', files('c5'));
$node->stop('immediate');
$node->start;
is($node->safe_psql('c5', $reads), '150 of 150', 'crash recovery replays the copies');
is($node->safe_psql('postgres', files('c5')), $c5, 'its files as they were');

# A standby restarted from a restartpoint before a CREATE DATABASE whose
# template was dropped since: the template's files are gone, the copies'
# whole.
template('tpl2');
$node->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
createdb('CREATE DATABASE c6 TEMPLATE tpl2');
$node->safe_psql('postgres', 'DROP DATABASE tpl2');
$node->wait_for_catchup($standby);
$standby->stop('immediate');
$standby->start;
$node->wait_for_catchup($standby);
is($standby->safe_psql('c6', $reads), '150 of 150',
	'a standby restarted after its template was dropped reads the copies');
is($standby->safe_psql('postgres', files('c6')), $node->safe_psql('postgres', files('c6')),
	'its files the primary\'s');

# While an online backup runs, the bytes are logged.
my $b = $node->background_psql('postgres');
$b->query_safe(q{SET client_min_messages = warning;
	SELECT pg_backup_start('createdb', true)});
my $in_backup = createdb('CREATE DATABASE c7 TEMPLATE tpl');
$b->query_safe(q{SET client_min_messages = warning;
	SELECT pg_backup_stop(false)});
$b->quit;
cmp_ok($in_backup - $copies, '>', $bytes * 0.9,
	"while a backup runs, the bytes are logged ($in_backup bytes of WAL)");
is($node->safe_psql('c7', $reads), '150 of 150', 'and the copy reads');
$node->wait_for_catchup($standby);
is($standby->safe_psql('c7', $reads), '150 of 150', 'on the standby too');

done_testing();
