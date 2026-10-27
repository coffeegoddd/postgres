# Copyright (c) 2026, PostgreSQL Global Development Group

# A move from a dirty working set keeps its comparison fold in the
# backend's memory and persists only what the baseline after reaches.  So
# after the server stops at once (no shutdown checkpoint) and recovers,
# everything the refs name, the baseline's trees among them, is in the
# store: a walk of the refs finds every chunk, the working set reads as
# the head's, a full collection keeps what is reached, and the next
# commit folds from the baseline.  Both ways: the baseline advanced to the
# target, and (the definitions moved too) not advanced, the fold's trees
# persisted with the values they name.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('compared_fold_restart');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
debug_prolly_page_roots = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text, big text STORAGE EXTERNAL);
	INSERT INTO s.t SELECT g, 'v' || g, 'small' FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'base');
});

# Dirty rows, with new long values out of line; a hard reset; the server
# stopped at once and started again; then everything checked.
sub reset_and_restart
{
	my ($what, $salt) = @_;

	sql(qq{
		UPDATE s.t SET v = 'dirty',
			big = (SELECT string_agg(md5(id || '$salt' || i), '')
				   FROM generate_series(1, 300) i)
		WHERE id % 5 = 0;
		SELECT dolt_reset('--hard');
	});
	$node->stop('immediate');
	$node->start;

	cmp_ok(sql(q{SELECT test_refs_walk_count(NULL)}), '>', 0,
		"$what: the refs walk finds every chunk");
	is(sql(q{SELECT count(*) FROM dolt_status}), '0', "$what: clean");
	is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', NULL::s.t)}),
		'0', "$what: no rows differ");
	is(sql(q{SELECT count(*) FROM s.t WHERE v = 'dirty' OR big <> 'small'}),
		'0', "$what: the rows put back");
	is(sql(q{SELECT test_fold_root(true) = prolly_root_hash('main')}),
		't', "$what: the working set folds to the head");
	cmp_ok(sql(q{SELECT kept FROM test_chunk_gc(true)}), '>', 0,
		"$what: a full collection");
	cmp_ok(sql(q{SELECT test_refs_walk_count(NULL)}), '>', 0,
		"$what: and the walk after it");
	sql(qq{
		UPDATE s.t SET v = 'after $what' WHERE id = 3;
		SELECT dolt_commit('-am', 'after $what');
	});
	is( sql(q{SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::s.t)}),
		'1', "$what: the next commit holds only its own row");
	is(sql(q{SELECT test_fold_root(true) = prolly_root_hash('main')}),
		't', "$what: and folds to its root");
}

reset_and_restart('advanced', ':');

# (a column added since the head: the baseline can't be advanced)
sql(q{ALTER TABLE s.t ADD COLUMN w int});
reset_and_restart('not advanced', '/');

$node->stop;
done_testing();
