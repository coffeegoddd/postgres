# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection never frees a chunk a running transaction may still name.
# A transaction running as it begins may have read under an older root
# (a commit whose refs changes are built on the refs as it last read them,
# while other branches' commits move the root on): the collection waits
# for it before marking from what is new.  A transaction begun after that
# which finds a chunk stored and so doesn't write it (a tree's node, or a
# pending chunk the store holds already) names one the collection keeps.
# A chunk whose bytes merely hold a collected chunk's hash, not as a
# reference, is no reason to refuse a transaction straddling the swap.
# And many sessions committing, checking out and deleting branches while
# collections run back to back see no chunk go missing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('gc_inflight');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE gi');
$node->safe_psql('gi',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('gi', $_[0]); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

sub waiting
{
	my ($event, $what) = @_;
	$node->poll_query_until('gi', qq{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = '$event'}) or die "$what";
}

# A collection in the background, stopped at point (if given).
sub collect
{
	my ($point) = @_;
	my $g = $node->background_psql('gi');
	if ($point)
	{
		$g->query_safe(qq{SELECT injection_points_set_local();
			SELECT injection_points_attach('$point', 'wait')});
	}
	$g->query_until(qr/started/, q{
		\echo started
		SELECT test_chunk_gc(false) IS NOT NULL;
		\echo done
	});
	waiting($point, "the collection did not stop at $point") if $point;
	return $g;
}

sub collected
{
	my ($g) = @_;
	$g->query_until(qr/done/, '');
	$g->quit;
}

# 1. A commit paused as it publishes, its refs changes built on the root
# it read before another branch's commit moved it: the collection waits
# for it, and it publishes.
sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'base')});
sql(q{SELECT dolt_commit('-Am', 'base')});
# (refs of many leaves: commits' records, whose keys are their ids, so each
# commit rewrites a leaf anywhere among them)
sql(q{DO $$ BEGIN FOR i IN 1 .. 300 LOOP
	PERFORM dolt_commit('--allow-empty', '-m', 'filler ' || i); END LOOP; END $$});
sql(q{SELECT dolt_branch('a'); SELECT dolt_branch('b')});
my $a = $node->background_psql('gi', on_error_stop => 0);
$a->query_safe(q{SELECT dolt_checkout('a')});
$a->query_safe(q{UPDATE s.t SET v = 'on a'});
$a->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-refs-publishing', 'wait')});
$a->query_until(qr/committing/, q{
	\echo committing
	SELECT dolt_commit('-am', 'on a') IS NOT NULL;
	\echo committed
});
waiting('prolly-refs-publishing', 'the commit did not stop as it publishes');
my $b = $node->background_psql('gi');
$b->query_safe(q{SELECT dolt_checkout('b')});
for my $i (1 .. 20)
{
	$b->query_safe(qq{UPDATE s.t SET v = 'on b $i'});
	$b->query_safe(qq{SELECT dolt_commit('-am', 'on b $i')});
	$b->query_safe(qq{SELECT dolt_branch('gone$i'); SELECT dolt_branch('-D', 'gone$i')});
}
my $g = collect();
waiting('virtualxid', 'the collection did not wait for the commit');
sql(q{SELECT injection_points_wakeup('prolly-refs-publishing')});
$a->query_until(qr/committed/, '');
is($a->{stderr}, '', 'a commit built on a root moved since publishes across a collection');
collected($g);
is(sql(q{SELECT message FROM dolt_log('a') LIMIT 1}), 'on a', 'and its commit is the head');
$a->quit;
$b->quit;

# The same in the store's terms: a transaction reads a tree under the
# root as it began, the root moves on (the tree is garbage now), and a
# collection runs; the transaction then names what it read.  The
# collection waits for it before marking what is new, so it is kept.
# (Each root holds the refs' too, put back after.)
my $refs = sql(q{SELECT test_chunk_root()});
my $old_leaf = sql(q{SELECT test_chunk_put('old leaf')});
my $old_root = sql(qq{SELECT test_chunk_put_node(1, ARRAY['$old_leaf', '$refs'])});
sql(qq{SELECT test_chunk_move_root('$refs', '$old_root')});
my $r = $node->background_psql('gi', on_error_stop => 0);
$r->query_safe(qq{BEGIN; SELECT test_chunk_get('$old_root') IS NOT NULL});
my $new_leaf = sql(q{SELECT test_chunk_put('new leaf')});
my $new_root = sql(qq{SELECT test_chunk_put_node(1, ARRAY['$new_leaf', '$refs'])});
sql(qq{SELECT test_chunk_move_root('$old_root', '$new_root')});
$g = collect();
waiting('virtualxid', 'the collection did not wait for the reader');
$r->query(qq{SELECT test_chunk_put_node(1, ARRAY['$old_leaf']) IS NOT NULL; COMMIT});
is($r->{stderr}, '', 'a transaction naming what it read under the root it began with commits');
collected($g);
is(has($old_leaf), 't', 'and what it named is kept');
$r->quit;
sql(qq{SELECT test_chunk_move_root('$new_root', '$refs')});

# 2. A transaction begun after the collection waited, which finds a
# garbage chunk stored (has(), as a tree asks before writing a node) and
# names it rather than put it: the collection keeps it.
my $found = sql(q{SELECT test_chunk_put('found again')});
$g = collect('prolly-gc-running-waited');
my $w = $node->background_psql('gi', on_error_stop => 0);
$w->query_safe(qq{BEGIN; SELECT test_chunk_has('$found')});
sql(q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
waiting('virtualxid', 'the collection did not wait for the writer at its retire');
$w->query(qq{SELECT test_chunk_put_node(1, ARRAY['$found']) IS NOT NULL; COMMIT});
is($w->{stderr}, '', 'a chunk found stored during a collection and named commits');
$w->{stderr} = '';
collected($g);
is(has($found), 't', 'and the chunk is kept');

# 3. The same for a pending chunk the store holds already, dropped from
# what is appended as the transaction commits in the collection's window:
# appended after all once the swap took it.
my $again = sql(q{SELECT test_chunk_put('put again')});
$g = collect('prolly-gc-kept');
$w->query_safe(qq{BEGIN; SELECT test_chunk_put('put again') IS NOT NULL;
	SELECT test_chunk_put_node(1, ARRAY['$again']) IS NOT NULL});
$w->query_until(qr/committing/, q{
	\echo committing
	COMMIT;
	\echo committed
});
$node->poll_query_until('gi', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'LWLock' AND backend_type = 'client backend'})
  or die 'the writer did not wait for the window';
sql(q{SELECT injection_points_wakeup('prolly-gc-kept')});
$w->query_until(qr/committed/, '');
is($w->{stderr}, '', 'a chunk put again as the store holds it commits across the swap');
$w->{stderr} = '';
collected($g);
is(has($again), 't', 'and the chunk is there');

# 4. A transaction straddling a swap whose chunk's bytes hold a collected
# chunk's hash (as a length and the hash), not as a reference, commits.
my $orphan = sql(q{SELECT test_chunk_put('orphan')});
$g = collect('prolly-gc-running-waited');
$w->query_safe(q{BEGIN; SELECT test_chunk_root() IS NOT NULL});
sql(q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
waiting('virtualxid', 'the collection did not wait for the writer at its retire');
is(has($orphan), 'f', 'a chunk nothing reaches is collected');
$w->query(qq{SELECT test_chunk_put('\\x0120'::bytea || '\\x14'::bytea || decode('$orphan', 'hex')) IS NOT NULL; COMMIT});
is($w->{stderr}, '', 'bytes that spell a collected hash, not a reference, are no refusal');
$w->{stderr} = '';
collected($g);
$w->quit;

# 5. Sessions committing, checking out, using and deleting branches while
# collections run back to back: no chunk a transaction names goes missing.
my $log_start = -s $node->logfile;
sql(q{INSERT INTO s.t SELECT g, 'r' || g FROM generate_series(2, 500) g});
sql(q{SELECT dolt_commit('-am', 'rows')});
sql(qq{SELECT dolt_branch('task$_')}) for 1 .. 4;
my %scripts = (
	rollout => q{
\set task random(1, 4)
\set r random(1, 1000000000)
SELECT dolt_checkout('-b', 'r_' || :client_id || '_' || :r, 'task' || :task);
UPDATE s.t SET v = 'u' || :r WHERE id = (:r % 500) + 1;
SELECT dolt_commit('--allow-empty', '-am', 'rollout');
INSERT INTO s.t VALUES (1000 + :r % 1000000, 'new') ON CONFLICT DO NOTHING;
SELECT dolt_commit('--allow-empty', '-am', 'rollout 2');
SELECT dolt_checkout('task' || :task);
SELECT dolt_branch('-D', 'r_' || :client_id || '_' || :r);
},
	task => q{
\set task random(1, 4)
\set r random(1, 1000000000)
SELECT dolt_checkout('task' || :task);
UPDATE s.t SET v = 't' || :r WHERE id = (:r % 500) + 1;
SELECT dolt_commit('--allow-empty', '-am', 'task step');
},
	gc => q{
DO $$ BEGIN PERFORM dolt_gc(); EXCEPTION WHEN lock_not_available THEN NULL; END $$;
});
my @files;
for my $name (sort keys %scripts)
{
	my $path = $node->basedir . "/$name.sql";
	open(my $fh, '>', $path) or die "could not write $path: $!";
	print $fh $scripts{$name};
	close($fh);
	push @files, '-f', $path . ($name eq 'rollout' ? '@6' : $name eq 'task' ? '@2' : '@1');
}
my ($out, $err) = run_command(
	[ 'pgbench', '-n', '-c', '6', '-j', '6', '-T', '8', '--continue-on-error',
		'--failures-detailed', @files, $node->connstr('gi') ]);
note $out;
like($out, qr/number of transactions actually processed: [1-9]/,
	'branch work ran beside collections');
my $log = slurp_file($node->logfile, $log_start);
unlike($log,
	qr/collected while the transaction ran|is not in the chunk store|node missing from its chunk store|deadlock detected/,
	'no transaction lost a chunk to a collection, nor deadlocked with one');
like(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'gc_collections'}),
	qr/^[1-9]/, 'and collections ran');

# (and the store is whole: a full collection marks, and so reads, all the
# root reaches; each task branch's rows and log read)
sql(q{SELECT dolt_gc('--full')});
for my $t (1 .. 4)
{
	my $b = "task$t";
	like(sql("SELECT count(*) FROM dolt_at('" . $b . "', NULL::s.t)"),
		qr/^[1-9]/, "$b reads whole");
	like(sql("SELECT count(*) FROM (SELECT * FROM dolt_log('" . $b . "') OFFSET 0) l"),
		qr/^[1-9]/, "$b log reads whole");
}

# 6. Transfers beside a collection.  A transaction open as it begins that
# has used the store is waited for before the swap; one that never has
# isn't.  While it waits the collection lets the table file writers' lock
# go: a transfer begun meanwhile adds its file and commits, the collection
# still waiting, and the file is kept.  The transaction waited for is
# refused a table file at once (what it found stored, it found before the
# collection kept such chunks).
# (nothing left retiring, whose readers a collection waits for first)
sql(q{SELECT test_chunk_gc(false) IS NOT NULL});
my $idle = $node->background_psql('gi');
$idle->query_safe(q{BEGIN; SELECT 1});
my $g6 = collect('prolly-gc-running-waited');
pass('a collection waits for no transaction that never used the store');
$node->safe_psql('gi', q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
$idle->query_safe(q{COMMIT});
$idle->quit;
collected($g6);

my $user = $node->background_psql('gi', on_error_stop => 0);
$user->query_safe(q{BEGIN; SELECT test_chunk_has('00')});
my $g7 = $node->background_psql('gi');
$g7->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo done
});
waiting('virtualxid', "the collection did not wait for the store's user");
sql(q{SELECT test_chunk_table_file(ARRAY['beside a collection'::bytea])});
pass('a transfer adds its file while the collection waits');
is(sql(q{SELECT count(*) FROM pg_stat_activity WHERE wait_event = 'virtualxid'}), '1',
	'the collection still waiting');
$user->query(q{SELECT test_chunk_table_file(ARRAY['refused'::bytea])});
like($user->{stderr}, qr/the chunk store is being collected/,
	'the transaction the collection waits for is refused a table file at once');
$user->query(q{ROLLBACK});
$user->quit;
collected($g7);
is(has(sql(q{SELECT test_content_hash('beside a collection')})), 't',
	"the transfer's chunk is kept by the collection");

$node->stop;
done_testing();
