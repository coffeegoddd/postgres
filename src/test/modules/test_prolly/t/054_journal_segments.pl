# Copyright (c) 2026, PostgreSQL Global Development Group

# The journal's segments: a standby's are the primary's byte for byte; a
# crash keeps every segment the manifest names, and a root written over a
# PAD in a segment sealed meanwhile; a seal that fails before its manifest
# leaves no segment named, and the next one removes what it left.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('segments');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
$node->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('segments_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# the store's files, each with its md5
my $files = q{SELECT string_agg(f || ' ' || md5(pg_read_binary_file(d || '/' || f)), E'\n' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'postgres') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_%' AND f NOT LIKE '%.tmp'};
my $segments = q{SELECT string_agg(name || ':' || chunks, ' ' ORDER BY name)
	FROM test_chunk_segments()};
my $reads = q{SELECT count(*) FROM generate_series(1, 30) g
	WHERE convert_from(test_chunk_get(encode(sha256(convert_to('s' || g, 'UTF8')), 'hex')), 'UTF8')
		= 's' || g};

# Ten chunks a segment, the root moved in each.
my $root = '';
for my $i (0 .. 2)
{
	my $last = sql(qq{SELECT max(test_chunk_put(convert_to('s' || g, 'UTF8')))
		FROM generate_series($i * 10 + 1, $i * 10 + 10) g});
	my $h = sql(qq{SELECT test_chunk_put(convert_to('s' || ($i * 10 + 10), 'UTF8'))});
	my $expected = $root eq '' ? 'NULL' : "'$root'";
	is(sql("SELECT test_chunk_move_root($expected, '$h')"), 't', "root moved in segment $i");
	$root = $h;
	sql('SELECT test_chunk_seal()') if $i < 2;
}
is(sql($segments), 'prolly_journal:10 prolly_journal_0000000000000001:10 '
	. 'prolly_journal_0000000000000002:10', 'three segments');
is(sql($reads), '30', 'every chunk reads');

SKIP:
{
	skip 'injection points not supported by this build', 5
	  if $ENV{enable_injection_points} ne 'yes';
	sql('CREATE EXTENSION injection_points');

	# A root move whose PAD went to a segment sealed before its commit: the
	# ROOT record is written there, and is the root.
	my $h = sql(q{SELECT test_chunk_put('padded')});
	my $a = $node->background_psql('postgres');
	$a->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-root-padded', 'wait')});
	$a->query_until(qr/started/, qq{
		\\echo started
		SELECT test_chunk_move_root('$root', '$h');
		\\echo done
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-root-padded'}) or die 'the mover did not wait';
	sql('SELECT test_chunk_seal()');
	sql(q{SELECT injection_points_wakeup('prolly-root-padded')});
	$a->query_until(qr/done/, '');
	$a->quit;
	is(sql('SELECT test_chunk_root()'), $h, 'a root written into a sealed segment is the root');
	$root = $h;

	# A seal that fails between its segment and its manifest leaves the
	# segment unnamed; the next seal removes it, and takes its id.
	sql(q{SELECT injection_points_attach('prolly-segment-made', 'error')});
	my ($ret, $out, $err) = $node->psql('postgres', 'SELECT test_chunk_seal()');
	like($err, qr/error triggered for injection point prolly-segment-made/,
		'a seal fails before its manifest');
	sql(q{SELECT injection_points_detach('prolly-segment-made')});
	is(sql(q{SELECT count(*) FROM test_chunk_segments()}), '4', 'its segment isn\'t named');
	sql('SELECT test_chunk_seal()');
	is(sql(q{SELECT string_agg(name, ' ' ORDER BY name) FROM test_chunk_segments()}),
		'prolly_journal prolly_journal_0000000000000001 prolly_journal_0000000000000002 '
		. 'prolly_journal_0000000000000003 prolly_journal_0000000000000004',
		'the next seal takes its id');
	is(sql(q{SELECT count(*) FROM
		(SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'postgres') db,
		pg_ls_dir(d) f WHERE f LIKE 'prolly\_journal%'}), '5', 'no segment file left unnamed');
}

# The standby's segments are the primary's, and it reads them.
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $files), sql($files),
	'a standby\'s segments and manifest are the primary\'s, byte for byte');
is($standby->safe_psql('postgres', $reads), '30', 'and it reads them');
is($standby->safe_psql('postgres', 'SELECT test_chunk_root()'), $root, 'and the root');

# A crash keeps them.
sql(q{SELECT test_chunk_put('after')});
$node->stop('immediate');
$node->start;
is(sql($reads), '30', 'the chunks survive a crash');
is(sql('SELECT test_chunk_root()'), $root, 'and the root');
is(sql(q{SELECT test_chunk_has(encode(sha256('after'), 'hex'))}), 't',
	'and the newest segment\'s');

$standby->stop;
$node->stop;
done_testing();
