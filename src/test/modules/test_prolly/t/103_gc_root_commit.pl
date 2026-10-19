# Copyright (c) 2026, PostgreSQL Global Development Group

# A transaction that moves the root reserves its PAD in the segment a
# collection appends to after its seal, and commits as the collection's
# window opens.  The root it moves to may be a chunk already in a
# collected file (an older root, moved back to), garbage as of the seal,
# which the move doesn't append again.  The ROOT record is written in the
# PAD's place as the commit's append is written, so the window, which
# waits for the appends in flight, finds the ROOT and keeps its chunk; it
# never marks from a PAD that becomes a ROOT after it.  (#328)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('gc_root_commit');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE EXTENSION injection_points');

# The root moves to "older", then on to "newer": "older" is garbage.
my $root = sql('SELECT test_chunk_root()');
my $older = sql(q{SELECT test_chunk_put('older')});
is(sql('SELECT test_chunk_move_root(' . ($root eq '' ? 'NULL' : "'$root'")
	  . ", '$older')"), 't', 'the root moved to "older"');
my $newer = sql(q{SELECT test_chunk_put('newer')});
is(sql("SELECT test_chunk_move_root('$older', '$newer')"), 't',
	'and on to "newer"');

# The collection, stopped once sealed and marked from its root, before
# its window.
my $gc = $node->background_psql('postgres');
my $gc_pid = $gc->query_safe('SELECT pg_backend_pid()');
$gc->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-roots-scanned', 'wait')});
$gc->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', qq{SELECT wait_event = 'prolly-gc-roots-scanned'
	FROM pg_stat_activity WHERE pid = $gc_pid})
  or die 'the collection did not stop';

# The move back to "older", its PAD in the new segment, stopped in its
# commit once the commit record is flushed, before its last append (and
# root) is written.
my $mover = $node->background_psql('postgres');
$mover->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-root-committing', 'wait')});
$mover->query_until(qr/moving/, qq{
	\\echo moving
	BEGIN;
	SELECT test_chunk_move_root('$newer', '$older');
	COMMIT;
	\\echo moved
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-root-committing'})
  or die 'the move did not stop';

# The collection goes on to its window, and waits for the move: for its
# append in flight (or, were the ROOT written after the window, for the
# move's transaction before it retires what its swap dropped).
sql(q{SELECT injection_points_wakeup('prolly-gc-roots-scanned')});
$node->poll_query_until('postgres', qq{SELECT wait_event IN ('ProllyJournalAppend', 'virtualxid')
	FROM pg_stat_activity WHERE pid = $gc_pid})
  or die 'the collection did not wait for the move';
sql(q{SELECT injection_points_wakeup('prolly-root-committing')});
$mover->query_until(qr/moved/, '');
$mover->quit;
$gc->query_until(qr/collected/, '');
$gc->quit;

is(sql('SELECT test_chunk_root()'), $older, 'the root is the one moved back to');
is(sql("SELECT test_chunk_has('$older')"), 't', 'the collection kept its chunk');
is(sql("SELECT convert_from(test_chunk_get('$older'), 'UTF8')"), 'older',
	'which reads back');

# After a crash, redo writes the PAD and then the ROOT over it.
$node->stop('immediate');
$node->start;
is(sql('SELECT test_chunk_root()'), $older, 'the root after a crash');
is(sql("SELECT convert_from(test_chunk_get('$older'), 'UTF8')"), 'older',
	'and its chunk');

done_testing();
