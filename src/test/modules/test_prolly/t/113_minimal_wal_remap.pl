# Copyright (c) 2026, PostgreSQL Global Development Group

# A remap logs the page before the write that required the copy.  If the
# relation's new storage skips data WAL, commit must log its final contents
# so that recovery cannot overwrite synced pages with that older image.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('minimal_remap');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_level = minimal
max_wal_senders = 0
autovacuum = off
checkpoint_timeout = '1h'
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int);
	INSERT INTO app.t SELECT g, g FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'base');
});

# Exercise both the fsync and the WAL size choices. No scan after commit:
# hint-bit full-page images could hide the loss before the crash.
for my $threshold (0, 2048)
{
	$node->safe_psql('postgres', qq{
		SET wal_skip_threshold = $threshold;
		CHECKPOINT;
		BEGIN;
		TRUNCATE app.t;
		INSERT INTO app.t SELECT g, g FROM generate_series(1, 5000) g;
		SELECT dolt_commit('-am', 'folded after truncate', '--allow-empty');
		SAVEPOINT writes;
		INSERT INTO app.t VALUES (5001, 5001);
		UPDATE app.t SET v = -v WHERE id = 1;
		DELETE FROM app.t WHERE id = 2;
		RELEASE writes;
		COMMIT;
	});
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', q{
		SET enable_indexscan = off;
		SET enable_indexonlyscan = off;
		SET enable_bitmapscan = off;
		SELECT count(*), sum(id), sum(v) FROM app.t;
	}), '5000|12507499|12507497',
		"writes after a fold survive crash recovery, threshold $threshold");
	is($node->safe_psql('postgres', q{
		SET enable_seqscan = off;
		SELECT id, v FROM app.t WHERE id IN (1, 2, 5001) ORDER BY id;
	}), "1|-1\n5001|5001", 'index lookups agree with the recovered rows');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM dolt_status'), '1',
		'the writes after the fold still leave the table modified');
}

# Newly created storage that was also truncated in place needs both its
# physical length synced and its post-remap contents logged.
$node->safe_psql('postgres', q{
	SET wal_skip_threshold = 0;
	CHECKPOINT;
	BEGIN;
	CREATE TABLE app.created (id int PRIMARY KEY, v int);
	INSERT INTO app.created SELECT g, g FROM generate_series(1, 10000) g;
	TRUNCATE app.created;
	INSERT INTO app.created SELECT g, g FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'new table folded after in-place truncate');
	INSERT INTO app.created VALUES (5001, 5001);
	UPDATE app.created SET v = -v WHERE id = 1;
	DELETE FROM app.created WHERE id = 2;
	COMMIT;
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{
	SELECT count(*), sum(id), sum(v) FROM app.created;
}), '5000|12507499|12507497',
	'new storage that was truncated in place keeps its post-fold writes');

# A repeatable-read first use empties a branch copy whose rows include
# changes committed after the session's snapshot. Its replacement storage
# skips WAL, just like a TRUNCATE's. This table exceeds the default WAL
# threshold as well as exercising the explicitly-zero threshold above.
$node->safe_psql('postgres', q{
	CREATE TABLE app.large (id int PRIMARY KEY, v int);
	INSERT INTO app.large SELECT g, g FROM generate_series(1, 60000) g;
	SELECT dolt_commit('-Am', 'large base');
	SELECT dolt_branch('rr');
	CHECKPOINT;
});
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres', 'INSERT INTO app.large VALUES (60002, 60002)');
$reader->query_safe(q{
	SELECT dolt_checkout('rr');
	INSERT INTO app.large VALUES (60001, 60001);
	UPDATE app.large SET v = -v WHERE id = 1;
	COMMIT;
});
$reader->quit;
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres/rr', q{
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*), sum(id), sum(v) FROM app.large;
}), '60001|1800090001|1800089999',
	'repeatable-read first-use writes survive crash recovery');
is($node->safe_psql('postgres/rr', q{
	SET enable_seqscan = off;
	SELECT id, v FROM app.large WHERE id IN (1, 60001, 60002) ORDER BY id;
}), "1|-1\n60001|60001", 'the branch index finds the committed insert and update');
is($node->safe_psql('postgres', q{
	SELECT id, v FROM app.large WHERE id IN (1, 60001, 60002) ORDER BY id;
}), "1|1\n60002|60002", 'the source branch keeps its own rows');
$node->stop;
done_testing();
