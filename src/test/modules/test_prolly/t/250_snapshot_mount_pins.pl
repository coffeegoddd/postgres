# Copyright (c) 2026, PostgreSQL Global Development Group

# A snapshot mount's pin against collections: a commit no branch names
# any more is kept while a mount pins it (a collection waits for every
# transaction in progress, so a scan can't be under way through one); an
# unmount waits for a scan in progress, then drops the pin.  A refresh
# likewise waits for the scans of its tables.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('snapshot_mount_pins');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	ALTER TABLE app.t ALTER v SET STORAGE EXTERNAL;
	SELECT dolt_commit('-Am', 'base');
});

# A commit on a branch then deleted: without a pin, a collection frees it;
# with one, mounted before the branch is deleted, it is kept.
sub side_commit
{
	my ($name) = @_;
	return sql(qq{
		SELECT dolt_branch('$name');
		SELECT dolt_checkout('$name');
		INSERT INTO app.t SELECT g, repeat(md5(g::text || '$name'), 100)
			FROM generate_series(1, 2000) g;
		SELECT dolt_commit('-Am', '$name rows');
		SELECT dolt_checkout('main');
		SELECT dolt_hashof('$name');
	}) =~ s/.*\n//sr;
}
my $loose = side_commit('loose');
sql(q{SELECT dolt_branch('-D', 'loose')});
sql(q{SELECT dolt_gc('--full')});
is(sql(qq{SELECT test_chunk_has('$loose')}), 'f', 'an unpinned commit no branch names is freed');

my $commit = side_commit('side');
sql(qq{SELECT prolly_mount('$commit', 'app', 'snap')});
sql(q{SELECT dolt_branch('-D', 'side')});
sql(q{SELECT dolt_gc('--full')});
is(sql(qq{SELECT test_chunk_has('$commit')}), 't', 'a collection keeps the pinned commit');
is(sql(q{SELECT count(*), sum(length(v)) FROM snap.t}), '2000|6400000',
	'and its rows read whole after it');

# a scan in progress, part way through its rows
my $reader = $node->background_psql('postgres');
$reader->query_safe(q{BEGIN; DECLARE c CURSOR FOR SELECT id, length(v) FROM snap.t ORDER BY id});
is($reader->query_safe(q{FETCH 3 FROM c}), "1|3200\n2|3200\n3|3200", 'a scan begins');

# an unmount waits for the scan to end
my $unmounter = $node->background_psql('postgres');
$unmounter->query_until(qr/started/, q{\echo started
SELECT prolly_unmount('snap');
\echo unmounted
});
ok( $node->poll_query_until('postgres',
		q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}),
	'the unmount waits for the scan');
is(sql(qq{SELECT test_chunk_has('$commit')}), 't', 'kept while it waits');
is($reader->query_safe(q{FETCH 2 FROM c}), "4|3200\n5|3200", 'the scan goes on');
is($reader->query_safe(q{MOVE ALL FROM c}), '', 'and reads the rest');
$reader->query_safe(q{COMMIT});
$unmounter->query_until(qr/unmounted/, '');
is(sql(q{SELECT count(*) FROM prolly_mounts()}), '0', 'the unmount ends after the scan');


# a refresh waits for scans of its tables too, then reads the new commit
sql(q{
	INSERT INTO app.t VALUES (1, 'one');
	SELECT dolt_commit('-Am', 'one row');
	SELECT prolly_mount('main', 'app', 'live');
});
$reader->query_safe(q{BEGIN; DECLARE d CURSOR FOR SELECT id FROM live.t});
is($reader->query_safe(q{FETCH 1 FROM d}), '1', 'a scan of the mount');
sql(q{
	INSERT INTO app.t VALUES (2, 'two');
	SELECT dolt_commit('-Am', 'two rows');
});
my $refresher = $node->background_psql('postgres');
$refresher->query_until(qr/started/, q{\echo started
SELECT prolly_refresh_mount('live');
\echo refreshed
});
ok( $node->poll_query_until('postgres',
		q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}),
	'the refresh waits for the scan');
$reader->query_safe(q{COMMIT});
$refresher->query_until(qr/refreshed/, '');
is(sql(q{SELECT count(*) FROM live.t}), '2', 'the refreshed mount reads the new commit');

$reader->quit;
$unmounter->quit;
$refresher->quit;

$node->stop;
done_testing();
