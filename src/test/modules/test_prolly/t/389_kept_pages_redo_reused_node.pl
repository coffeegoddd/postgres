# Copyright (c) 2026, PostgreSQL Global Development Group

# Redo of a snapshot walks the nodes its record names, which by the end of
# the WAL may be gone: the snapshot let go of (prolly.kept_commits), its
# leaf freed, and the block taken as a relation's page and written out
# before a crash.  A relation's page is checksummed as its logical block,
# so read through a buffer as the page store's block it fails
# verification, an error that would stop recovery on every restart.  The
# walk reads such a block without a buffer, and at a page that doesn't
# verify writes out every buffer of the relation instead.  Without data
# checksums, which would show a torn page, redo doesn't walk at all.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_redo_reused_node');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_buffers = 16MB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
is(sql('SHOW data_checksums'), 'on', 'data checksums are on');

sql(q{
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.t SELECT g, 0, repeat('x', 100) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
});

# the leaf copied before the checkpoint, kept by a commit after it
sql(q{UPDATE app.t SET v = 1 WHERE id = 100});
sql('CHECKPOINT');
sql(q{SELECT dolt_commit('-am', 'c1')});

# c1 let go of (two commits kept), its leaf freed, and taken as data pages
for my $r (2 .. 4)
{
	sql(qq{UPDATE app.t SET v = $r WHERE id = $r * 100});
	sql(qq{SELECT dolt_commit('-am', 'c$r')});
}
sql('VACUUM app.t');
sql(q{INSERT INTO app.t SELECT g, 0, repeat('y', 100) FROM generate_series(2001, 4000) g});
sql(q{SELECT pg_buffercache_evict_all()});
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};
my $now = sql($digest);

my $log_offset = -s $node->logfile;
$node->stop('immediate');
ok($node->start(fail_ok => 1), 'recovery completes');
ok( !$node->log_contains('invalid page in block', $log_offset),
	'reading no page through a buffer that fails verification');
ok( $node->log_contains(
		'redo of a snapshot of .* writes out every buffer of it: a page of its map or counts read is newer than the replay, unverified, or not the node named',
		$log_offset),
	'a snapshot whose leaf is gone written out by every buffer');
is(sql($digest), $now, 'the rows as they were');


# Without data checksums a torn page goes unseen, so redo doesn't walk:
# it writes out every buffer of the relation, as before the walk
my $plain = PostgreSQL::Test::Cluster->new('kept_pages_redo_no_checksums');
$plain->init(extra => ['--no-data-checksums']);
$plain->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
log_min_messages = debug1
});
$plain->start;
$plain->safe_psql('postgres', q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int);
	INSERT INTO app.t SELECT g, 0 FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base');
	CHECKPOINT;
	UPDATE app.t SET v = 1 WHERE id = 1;
	SELECT dolt_commit('-am', 'c1');
});
$log_offset = -s $plain->logfile;
$plain->stop('immediate');
$plain->start;
ok( $plain->log_contains(
		'redo of a snapshot of .* writes out every buffer of it: data checksums are off',
		$log_offset),
	'without data checksums, redo of a snapshot writes out every buffer');
is($plain->safe_psql('postgres', q{SELECT count(*) FROM app.t WHERE v = 1}), '1',
	'the rows as they were');
$plain->stop;

$node->stop;
done_testing();
