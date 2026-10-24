# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's pages kept by a snapshot are written out as it is taken, but
# only those changed since the relation's last snapshot: the pages its walk
# of its own map finds, each looked up in the buffers.  A page dirty at the
# commit that never changes afterwards is never copied, so nothing else
# would write it out, and a relation made of the snapshot reads its block
# from disk: a hard reset that gives the kept pages back (dropping the
# working set's storage, and that dirty buffer with it), and a branch
# first used of them.  No crash is needed to lose its rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_unwritten');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_buffers = 64MB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
prolly.restore_min_changes = 0
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

# (many pages, written out: each round's changes are only in buffers; a
# second index and a TOAST table, kept too)
sql(q{
	CREATE EXTENSION amcheck;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, k int, pad text, big text)
		WITH (fillfactor = 10);
	CREATE INDEX t_k ON app.t (k);
	INSERT INTO app.t SELECT g, 0, g, repeat('x', 100), NULL
		FROM generate_series(1, 2000) g;
	UPDATE app.t SET big = (SELECT string_agg(md5(id || ':' || i), '')
		FROM generate_series(1, 200) i) WHERE id <= 40;
	SELECT dolt_commit('-Am', 'base');
	CHECKPOINT;
});
my $digest = q{SELECT md5(string_agg(id || ':' || v || ':' || k || ':' || md5(coalesce(big, '')), ','
	ORDER BY id)) FROM app.t};

# the same rows read through the second index alone, and both indexes checked
sub through_index
{
	my ($db) = @_;
	return $node->safe_psql($db, q{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT md5(string_agg(id || ':' || v || ':' || k || ':' || md5(coalesce(big, '')), ','
			ORDER BY id)) FROM (SELECT * FROM app.t WHERE k > 0 ORDER BY k) r;
		SELECT bt_index_check('app.t_k', true);
		SELECT bt_index_check('app.t_pkey', true);
	});
}

for my $round (1 .. 3)
{
	# rows of the first pages changed (their indexed key and toasted value
	# too) and committed: those pages kept, dirty
	sql(qq{UPDATE app.t SET v = $round, k = k + 10000,
		big = (SELECT string_agg(md5(id || ':' || i || ':$round'), '')
			FROM generate_series(1, 200) i)
		WHERE id <= 40});
	sql(qq{SELECT dolt_commit('-am', 'round $round')});
	my $at = sql($digest);
	my $at_index = through_index('postgres');
	sql(qq{SELECT dolt_branch('b$round')});

	# only a page far from them changes: the kept ones are never copied
	sql(qq{UPDATE app.t SET v = -1 WHERE id = 2000});
	isnt(sql($digest), $at, "round $round: the working set changed");

	# a branch first used of the kept pages reads them from their blocks
	is($node->safe_psql('postgres/b' . $round, $digest), $at,
		"round $round: a branch made of them has the commit's rows");
	is(through_index('postgres/b' . $round), $at_index,
		"round $round: and its indexes, checked");

	my $restored = counted('relations_restored');
	sql(q{SELECT dolt_reset('--hard')});
	cmp_ok(counted('relations_restored'), '>', $restored,
		"round $round: the reset gave the kept pages back");
	is(sql($digest), $at, "round $round: and they hold the commit's rows");
	is(through_index('postgres'), $at_index,
		"round $round: through its indexes too, checked");
}

$node->stop;
done_testing();
