# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's roots captured among writers (dolt.transaction_commit) are
# kept unfrozen, and frozen on a move to the commit, as of its snapshot.
# A restore of them then gives the table a visibility map whose bits come
# from the frozen pages' all-visible flags, so those flags must be the
# freezer's own: a page holding a version of a transaction in progress at
# the capture (made dead by the freezing, its index entry still there)
# is never flagged all-visible, nor does a page keep the live page's
# flag.  So an index-only scan doesn't take the in-progress row for one
# of the commit's, and the map is true of the pages.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lazy_frozen_vm');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT coalesce((SELECT value FROM pg_stat_prolly
		WHERE name = '$name'), 0)});
}

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM lv.t t};
my $clean = q{SELECT (SELECT count(*) FROM pg_check_frozen('lv.t')) +
	(SELECT count(*) FROM pg_check_visible('lv.t'))};
my $ios = q{SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM lv.t WHERE v = 777};

sql(q{CREATE EXTENSION pg_visibility;
	CREATE SCHEMA lv VERSIONED;
	CREATE TABLE lv.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
	CREATE INDEX t_v ON lv.t (v);
	INSERT INTO lv.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 3000) g;
	VACUUM lv.t;
	SELECT dolt_commit('-Am', 'base')});
sql('CHECKPOINT');

# a writer in progress as the next commit's roots are captured: its rows
# on the table's pages, and in its index
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN;
	INSERT INTO lv.t SELECT g, 777, 'in progress' FROM generate_series(100001, 100200) g;
	UPDATE lv.t SET v = 777 WHERE id % 50 = 0});
my $before = counted('roots_frozen_later');
sql(q{SET dolt.transaction_commit = on; SET prolly.root_interval = 0;
	UPDATE lv.t SET v = 1 WHERE id % 10 = 1});
my $at_c1 = sql($digest);
$writer->query_safe('COMMIT');
$writer->quit;
is(sql($ios), '260', 'the writer\'s rows, committed after the commit, in the working set');

# the working set dirtied, then reset to the commit: its roots frozen as
# of its snapshot, which doesn't see the writer, and given back
sql(q{UPDATE lv.t SET v = -1 WHERE id <= 2000});
my $restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard'); SELECT pg_stat_force_next_flush()});
ok(counted('roots_frozen_later') > $before, 'the commit\'s roots were frozen at the move');
ok(counted('relations_restored') > $restored, 'and the table restored from them');
is(sql($digest), $at_c1, 'the commit\'s rows');
like(sql(q{SET enable_seqscan = off; SET enable_bitmapscan = off;
	EXPLAIN (COSTS OFF) SELECT count(*) FROM lv.t WHERE v = 777}), qr/Index Only Scan/,
	'read by an index-only scan');
is(sql($ios), '0', 'which takes none of the writer\'s rows for the commit\'s');
ok(sql(q{SELECT all_visible FROM pg_visibility_map_summary('lv.t')}) > 0,
	'the table has a visibility map');
is(sql($clean), '0', 'true of the pages');

$node->stop;
done_testing();
