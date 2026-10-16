# Copyright (c) 2026, PostgreSQL Global Development Group

# The chunk store's table files and manifest: a standby's are the
# primary's byte for byte; a crash keeps the files the manifest names; a
# writer that fails before its manifest leaves no file named, and the next
# one writes over what it left; recovery stopped at every record boundary
# has the files of the last manifest before it; a base backup restores
# them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('tables');
$node->init(allows_streaming => 1, has_archiving => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION pg_walinspect');
$node->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('tables_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# the store's files, each with its md5
my $files = q{SELECT string_agg(f || ' ' || md5(pg_read_binary_file(d || '/' || f)), E'\n' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'postgres') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_%' AND f NOT LIKE '%.tmp'};
my $named = q{SELECT coalesce(string_agg(name || ':' || chunks, ' ' ORDER BY name), '')
	FROM test_chunk_table_files()};
my $big = q{SELECT count(*) FROM generate_series(1, 40) g
	WHERE convert_from(test_chunk_get(encode(sha256(convert_to(repeat(g::text, 9000), 'UTF8')), 'hex')), 'UTF8')
		= repeat(g::text, 9000)};
sub has { return sql("SELECT test_chunk_has(encode(sha256('$_[0]'::bytea), 'hex'))"); }

my $start = sql('SELECT pg_current_wal_insert_lsn()');
my @manifests;	# the files named after each manifest, in order

sql(q{SELECT test_chunk_table_file(ARRAY['a1'::bytea, 'a2'::bytea])});
push @manifests, sql($named);
sql(q{SELECT test_chunk_table_file(array_agg(convert_to(repeat(g::text, 9000), 'UTF8')))
	FROM generate_series(1, 40) g});
push @manifests, sql($named);
sql('CHECKPOINT');
sql(q{SELECT test_chunk_table_file(ARRAY['c1'::bytea])});
push @manifests, sql($named);
is($manifests[2], 'prolly_table_0000000000000001:2 prolly_table_0000000000000002:40 '
	. 'prolly_table_0000000000000003:1', 'each table file is named');

# A session reading the files while another conjoins them.
my $reader = $node->background_psql('postgres');
is($reader->query_safe(q{SELECT convert_from(test_chunk_get(encode(sha256('a1'::bytea), 'hex')), 'UTF8')}),
	'a1', 'a session reads a table file');
sql('SELECT test_chunk_conjoin()');
push @manifests, sql($named);
is($manifests[3], 'prolly_table_0000000000000004:43', 'conjoined as one');
is($reader->query_safe(q{SELECT convert_from(test_chunk_get(encode(sha256('c1'::bytea), 'hex')), 'UTF8')}),
	'c1', 'and reads on from the new one');
$reader->quit;
is(sql($big), '40', 'every chunk reads back');
my $end = sql('SELECT pg_current_wal_insert_lsn()');

# A writer that fails before its manifest leaves no file named.
SKIP:
{
	skip 'injection points not supported by this build', 4
	  if $ENV{enable_injection_points} ne 'yes';

	sql('CREATE EXTENSION injection_points');
	sql(q{SELECT injection_points_attach('prolly-table-file-written', 'error')});
	my ($ret, $out, $err) = $node->psql('postgres',
		q{SELECT test_chunk_table_file(ARRAY['e1'::bytea])});
	like($err, qr/error triggered for injection point prolly-table-file-written/,
		'a writer fails after its file, before its manifest');
	sql(q{SELECT injection_points_detach('prolly-table-file-written')});
	is(sql($named), $manifests[3], 'its file isn\'t named');
	is(has('e1'), 'f', 'nor its chunk found');
	sql(q{SELECT test_chunk_table_file(ARRAY['e2'::bytea])});
	is(sql($named), 'prolly_table_0000000000000004:43 prolly_table_0000000000000005:1',
		'the next writer writes over what it left');
}

# The standby's files are the primary's.
# (to the WAL inserted: the files come from transactions without an xid,
# whose commits don't flush it)
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $files), sql($files),
	'a standby\'s table files and manifest are the primary\'s, byte for byte');
is($standby->safe_psql('postgres', $big), '40', 'and it reads them');

# A crash keeps what the manifest names.
$node->stop('immediate');
$node->start;
is(sql($big), '40', 'the chunks survive a crash');
is(has('a1'), 't', 'those of conjoined files too');

# (every segment archived, for recovery from the archive)
my $seg = sql('SELECT pg_walfile_name(pg_current_wal_lsn())');
sql('SELECT pg_switch_wal()');
$node->poll_query_until('postgres',
	"SELECT last_archived_wal >= '$seg' FROM pg_stat_archiver")
  or die 'the WAL was not archived';

# Recovery stopped at each record boundary of the files: the files named
# are those of the last manifest before it, and read.
my @boundaries = split /\n/, sql(qq{
	SELECT end_lsn FROM pg_get_wal_records_info('$start', '$end')
	WHERE resource_manager = 'ProllyChunks' AND record_type <> 'TYPE_1'
	ORDER BY end_lsn});
my @manifest_ends = split /\n/, sql(qq{
	SELECT end_lsn FROM pg_get_wal_records_info('$start', '$end')
	WHERE resource_manager = 'ProllyChunks' AND record_type = 'TYPE_3'
	ORDER BY end_lsn});
is(scalar(@manifest_ends), scalar(@manifests), 'each manifest has its record');
cmp_ok(scalar(@boundaries), '>', 8, 'records to stop at');
my $i = 0;
my $bad = 0;
for my $lsn (@boundaries, $manifest_ends[0])
{
	my $want = '';
	for my $m (0 .. $#manifest_ends)
	{
		$want = $manifests[$m]
		  if sql("SELECT '$manifest_ends[$m]'::pg_lsn <= '$lsn'::pg_lsn") eq 't';
	}
	my $pitr = PostgreSQL::Test::Cluster->new('tables_pitr' . ++$i);
	$pitr->init_from_backup($node, 'base', has_restoring => 1);
	$pitr->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_inclusive = off
recovery_target_action = 'promote'
recovery_target_timeline = 'current'
archive_mode = off
});
	$pitr->start;
	$pitr->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die "recovery to $lsn did not end";
	my $got = $pitr->safe_psql('postgres', $named);
	my $reads = $pitr->safe_psql('postgres',
		q{SELECT test_chunk_has(encode(sha256('a1'::bytea), 'hex'))});
	if ($got ne $want || $reads ne ($want eq '' ? 'f' : 't'))
	{
		diag("stopped at $lsn: files <$got>, expected <$want>; a1 found: $reads");
		$bad++;
	}
	$pitr->stop;
}
is($bad, 0, 'recovery to every record boundary has the last manifest\'s files');

# A base backup restores them.
$node->backup('later');
my $restored = PostgreSQL::Test::Cluster->new('tables_restored');
$restored->init_from_backup($node, 'later');
$restored->start;
is($restored->safe_psql('postgres', $files), sql($files),
	'a base backup has the table files and manifest');
is($restored->safe_psql('postgres', $big), '40', 'and reads them');
$restored->stop;

$standby->stop;
$node->stop;
done_testing();
