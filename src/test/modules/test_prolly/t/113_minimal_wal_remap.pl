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
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int);
	INSERT INTO app.t SELECT g, g FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'base');
});

# The rows after the writes below: n rows, then n + 1 inserted, 2 deleted
# and 1 negated.
sub expected_sums
{
	my ($n) = @_;
	my $sum = $n * ($n + 1) / 2 + ($n + 1) - 2;
	return "$n|$sum|" . ($sum - 2);
}

# Exercise both the fsync and the WAL size choices: the default threshold's
# table, about 4 MB, is well over it, so only the remap requires its WAL.
# Logging the pages at commit changes none of their rows, so it remaps none
# of them.  No scan after commit: hint-bit full-page images could hide the
# loss before the crash.
for my $case ([ 0, 5000 ], [ 2048, 120000 ])
{
	my ($threshold, $n) = @$case;
	my $m = $n + 1;

	is($node->safe_psql('postgres', qq{
		SET wal_skip_threshold = $threshold;
		CHECKPOINT;
		BEGIN;
		TRUNCATE app.t;
		INSERT INTO app.t SELECT g, g FROM generate_series(1, $n) g;
		SELECT dolt_commit('-am', 'folded after truncate', '--allow-empty') IS NOT NULL;
		SAVEPOINT writes;
		INSERT INTO app.t VALUES ($m, $m);
		UPDATE app.t SET v = -v WHERE id = 1;
		DELETE FROM app.t WHERE id = 2;
		RELEASE writes;
		CREATE TEMP TABLE before_commit AS
			SELECT blkno, phys FROM test_bmap_map(pg_relation_filenode('app.t'));
		COMMIT;
		SELECT count(*) FROM before_commit b
			JOIN test_bmap_map(pg_relation_filenode('app.t')) a USING (blkno)
			WHERE a.phys <> b.phys;
	}), "t\n0", "commit logs the pages without remapping them, threshold $threshold");
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', q{
		SET enable_indexscan = off;
		SET enable_indexonlyscan = off;
		SET enable_bitmapscan = off;
		SELECT count(*), sum(id), sum(v) FROM app.t;
	}), expected_sums($n),
		"writes after a fold survive crash recovery, threshold $threshold");
	is($node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT id, v FROM app.t WHERE id IN (1, 2, $m) ORDER BY id;
	}), "1|-1\n$m|$m", 'index lookups agree with the recovered rows');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM dolt_status'), '1',
		'the writes after the fold still leave the table modified');
}

# A remap's WAL outlives a savepoint rolled back, so commit must still log
# the page that the parent then writes again.
$node->safe_psql('postgres', q{
	SET wal_skip_threshold = 0;
	CHECKPOINT;
	BEGIN;
	TRUNCATE app.t;
	INSERT INTO app.t SELECT g, g FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-am', 'folded before a savepoint', '--allow-empty');
	SAVEPOINT remap;
	UPDATE app.t SET v = 0 WHERE id = 1;
	ROLLBACK TO remap;
	UPDATE app.t SET v = -v WHERE id = 1;
	COMMIT;
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*), sum(id), sum(v) FROM app.t;
}), '5000|12502500|12502498',
	'a write after a savepoint rolled back past a remap survives');

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

# The same, truncated in place to a shorter length after the remap: redo
# must not leave the remapped page's older image over the rows written in
# its place after.
$node->safe_psql('postgres', q{
	SET wal_skip_threshold = 0;
	CHECKPOINT;
	BEGIN;
	CREATE TABLE app.shrunk (id int PRIMARY KEY, v int);
	INSERT INTO app.shrunk SELECT g, g FROM generate_series(1, 10000) g;
	SELECT dolt_commit('-Am', 'new table folded before a remap');
	UPDATE app.shrunk SET v = -v WHERE id = 1;
	TRUNCATE app.shrunk;
	INSERT INTO app.shrunk SELECT g, 2 * g FROM generate_series(1, 5000) g;
	COMMIT;
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{
	SET enable_indexscan = off;
	SET enable_indexonlyscan = off;
	SET enable_bitmapscan = off;
	SELECT count(*), sum(id), sum(v) FROM app.shrunk;
}), '5000|12502500|25005000',
	'rows written after an in-place truncate past a remap survive');

# A repeatable-read first use empties a branch copy whose rows include
# changes committed after the session's snapshot. Its replacement storage
# skips WAL, just like a TRUNCATE's.
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
