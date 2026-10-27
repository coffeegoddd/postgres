# Copyright (c) 2026, PostgreSQL Global Development Group

# The background frees severed history by retiring the generations
# holding it, not by a full collection: in delete mode, an autovacuum
# visit severs past the window, then retires what the sever's estimates
# say is mostly garbage.  The fragment's data is freed, the settled files
# outside the retire stay as they are, and no page-store block is read
# for hash pointers (none changed since the last scan: the cached roots
# are read by generation).  A batch boundary passed since the newest
# generation ended has the visit settle what came since in a generation
# of its own, however little it is.  A retire that keeps failing never
# stops the store's collections.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gw');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE gw');

sub sql { return $node->safe_psql('gw', $_[0]); }

# An autovacuum worker's whole visit of the database (see 625).
sql(q{CREATE TABLE visits (n int) USING heap WITH
	(autovacuum_vacuum_insert_threshold = 0,
	 autovacuum_vacuum_insert_scale_factor = 0)});
sub autovacuum_visited
{
	my $count = q{SELECT coalesce(autovacuum_count, 0) FROM pg_stat_user_tables
		WHERE relname = 'visits'};
	my $before = sql($count);
	sql('INSERT INTO visits VALUES (1)');
	$node->poll_query_until('gw', "SELECT ($count) > $before")
	  or die 'autovacuum did not visit';
	$node->poll_query_until('postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
		WHERE backend_type = 'autovacuum worker' AND datname = 'gw'})
	  or die 'the autovacuum worker did not leave';
}

sql(q{CREATE EXTENSION test_prolly});
sql(q{CREATE SCHEMA gw VERSIONED;
	CREATE TABLE gw.t (id int PRIMARY KEY, v text);
	INSERT INTO gw.t SELECT g, (SELECT string_agg(md5(g::text || i), '')
		FROM generate_series(1, 100) i) FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'd10', '--date', (now() - interval '10 days')::text)});
sql(q{SELECT * FROM test_chunk_gc(false)});
my $old_root = sql(q{SELECT root FROM test_commit_roots('main')});
for my $ago (reverse 1 .. 9)
{
	sql(qq{UPDATE gw.t SET v = (SELECT string_agg(md5('$ago' || id || i), '')
			FROM generate_series(1, 100) i) WHERE id % 3 = $ago % 3;
		SELECT dolt_commit('-Am', 'd$ago', '--date', (now() - interval '$ago days')::text)});
	sql(q{SELECT * FROM test_chunk_gc(false)});
}
# (the table vacuumed now, and not by the visits: its pages change no more)
sql(q{ALTER TABLE gw.t SET (autovacuum_enabled = false)});
sql(q{VACUUM gw.t});
sql(q{SELECT * FROM test_chunk_gc(false)});
my $values = q{SELECT count(*) FROM gw.t WHERE length(v) = 3200};
is(sql($values), '200', 'the values, out of line');
my $newest_file = sql(q{SELECT name FROM test_chunk_generations()
	WHERE settled ORDER BY epoch_lo DESC LIMIT 1});
my $pages = q{SELECT value FROM pg_stat_prolly WHERE name = 'gc_pages_scanned'};
my $pages_before = sql($pages);
my $bytes = q{SELECT sum(bytes) FROM test_chunk_table_files()};
my $bytes_before = sql($bytes);

# Delete mode, a window of three days, batches of a second.
sql(q{ALTER DATABASE gw SET prolly.history_window = '3 days';
	ALTER DATABASE gw SET prolly.history_window_action = delete;
	ALTER DATABASE gw SET prolly.history_batch = '1 second';
	ALTER DATABASE gw SET client_min_messages = warning});
my $offset = -s $node->logfile;
autovacuum_visited();
is(sql(q{SELECT count(*) FROM prolly_history_horizon()}), '1', 'severed');
ok($node->log_contains(qr/automatic retire of generations \d+ to \d+ of database "gw"/,
	$offset), 'retired in the background');
ok(!$node->log_contains(qr/automatic (full )?collection of database "gw"/, $offset),
	'no collection but the retire');
is(sql($pages), $pages_before, 'no page-store block read for hash pointers');
is(sql(q{SELECT test_chunk_has(} . "'$old_root'" . q{)}), 'f',
	'the fragment\'s data freed');
ok(sql($bytes) < $bytes_before, 'the store smaller');
is(sql(qq{SELECT count(*) FROM test_chunk_generations() WHERE name = '$newest_file'}),
	'1', 'a settled file outside the retire left as it was');
is(sql($values), '200', 'the values read');
ok(sql(q{SELECT test_chunk_verify_generations() > 0}) eq 't', 'generations check out');

# A commit, then a batch boundary: the visit settles it on its own.
my $epochs = q{SELECT count(*) FROM test_chunk_epochs()};
my $epochs_before = sql($epochs);
sql(q{UPDATE gw.t SET v = 'now' WHERE id = 1; SELECT dolt_commit('-Am', 'now')});
sleep(2);
$offset = -s $node->logfile;
autovacuum_visited();
ok($node->log_contains(qr/automatic (collection|retire)[^\n]* "gw"/, $offset),
	'collected at the boundary');
ok(sql($epochs) > $epochs_before, 'in a generation of its own');

# A retire that keeps failing doesn't stop the store's collections: each
# visit's default collection runs all the same, the retire is tried again
# after a wait, and after three failures in a row a full collection runs
# instead.  (Batches of a day, so no seal collects meanwhile.)
SKIP:
{
	skip 'injection points not supported by this build', 6
	  if ($ENV{enable_injection_points} ne 'yes');
	sql(q{CREATE EXTENSION injection_points});
	sql(q{ALTER DATABASE gw SET prolly.history_batch = '1 day';
		ALTER DATABASE gw SET prolly.history_collect_interval = '1 second'});
	my $due = q{SELECT test_chunk_note_garbage(array(SELECT test_chunk_file_hashes(name)))
		FROM test_chunk_generations() WHERE settled ORDER BY epoch_lo DESC LIMIT 1};
	sql(q{UPDATE gw.t SET v = 'fail' WHERE id = 2; SELECT dolt_commit('-Am', 'fail')});
	sql($due);
	$offset = -s $node->logfile;
	sql(q{SELECT injection_points_attach('prolly-gc-retire-begin', 'error')});
	# (the visits come on their own, a second apart: the log read as it grows)
	my $log = sub { return substr(slurp_file($node->logfile), $offset); };
	my $full = qr/automatic full collection of database "gw"/;
	my $waited = 0;
	until ($log->() =~ $full || $waited++ > 120) { sleep(1); }
	my $text = $log->();
	my ($before) = split($full, $text, 2);
	my @failed = ($before =~ /worker\[(\d+)\] WARNING:  automatic retire of generations \d+ to \d+ of database "gw" failed/g);
	cmp_ok(scalar(@failed), '>=', 3, 'the retire failed, visit after visit');
	ok($before =~ /worker\[$failed[0]\] DEBUG:  automatic collection of database "gw"/,
		'the default collection ran after the first failure');
	ok($text =~ $full, 'a full collection after three failures');
	cmp_ok(scalar(@failed), '<=', 3, 'no more retires tried before it');
	sql(q{SELECT injection_points_detach('prolly-gc-retire-begin')});
	is(sql(q{SELECT count(*) FROM gw.t WHERE length(v) = 3200 OR id IN (1, 2)}),
		'200', 'the values read');
	ok(sql(q{SELECT test_chunk_verify_generations() > 0}) eq 't',
		'generations check out');
}

$node->stop;
done_testing();
