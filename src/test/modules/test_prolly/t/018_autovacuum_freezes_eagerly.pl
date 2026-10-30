# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum freezes a block-mapped table eagerly, as VACUUM does when no
# freeze age is given: storage that may share its pages freezes them while
# they are still its own, so a share made later needs no copy to freeze
# them.  A freeze age set explicitly still wins.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('autovacuum_freezes_eagerly');
$node->init;
# No checkpoint while the rows are new: a page's first image after one
# would let VACUUM freeze it opportunistically, eager or not.
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum_naptime = 1s
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION pg_visibility;
	CREATE TABLE eager (id int, pad text) WITH (
	  autovacuum_vacuum_insert_threshold = 100,
	  autovacuum_vacuum_insert_scale_factor = 0);
	CREATE TABLE explicit (id int, pad text) WITH (
	  autovacuum_vacuum_insert_threshold = 100,
	  autovacuum_vacuum_insert_scale_factor = 0,
	  autovacuum_freeze_min_age = 1000000);
	INSERT INTO eager SELECT g, repeat('e', 200) FROM generate_series(1, 3000) g;
	INSERT INTO explicit SELECT g, repeat('x', 200) FROM generate_series(1, 3000) g;
});

foreach my $table ('eager', 'explicit')
{
	ok( $node->poll_query_until(
			'postgres',
			qq{SELECT autovacuum_count > 0 FROM pg_stat_user_tables
	WHERE relname = '$table'}),
		"autovacuum vacuums $table");
}

my $nblocks = sql(q{SELECT pg_relation_size('eager') / 8192});
is( sql(
		q{SELECT count(*) FROM pg_visibility_map('eager')
	WHERE all_visible AND all_frozen}),
	$nblocks,
	'and freezes every page of a block-mapped table');
is( sql(
		q{SELECT count(*) FROM pg_visibility_map('explicit')
	WHERE all_frozen}),
	'0',
	'but not one whose freeze age is set');

$node->stop;
done_testing();
