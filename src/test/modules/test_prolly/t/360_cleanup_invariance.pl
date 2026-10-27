# Copyright (c) 2026, PostgreSQL Global Development Group

# Cleanup invariance: storage cleanup frees only what no read needs.  A
# history that shares storage every way it can (kept commits, some pushed
# out of the window, a kept detached checkout, a branch used and then
# deleted, a dropped table and one dropped and made again, a merge,
# snapshot and history mounts, dirty working sets, TOAST values) is read
# by a fixed set of queries, each result digested independent of row
# order.  Then each cleanup runs in turn (every VACUUM variant, the release
# queue drained by autovacuum, dolt_gc('--full'), dolt_gc(), the page
# sweep, a checkpoint, a restart and a crash), each shown to do its work
# (pages copied, blocks and chunks freed, counts recounted, blocks moved,
# shared_below lowered), and after each the blocks it freed are written
# again by another table and every buffer evicted, so that a read of
# storage it freed reads that table's pages: every digest must be what it
# was before any cleanup.  Folds still match their trees, every index still
# passes amcheck, and the server log has no errors or warnings.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('cleanup_invariance');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
autovacuum_naptime = 1
prolly_fold_worker = off
prolly.kept_commits = 4
});
# (roots sharing the relations' pages as they are: frozen ones share none)
$node->append_conf('postgresql.conf', 'prolly.freeze_roots = off');
$node->start;
my $log_start = -s $node->logfile;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $rows = 3000;
sql(qq{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION amcheck;
	CREATE EXTENSION pg_buffercache;
	CREATE SCHEMA ci VERSIONED;
	CREATE TABLE ci.t (id int PRIMARY KEY, k int NOT NULL, g int NOT NULL,
		v text NOT NULL);
	CREATE INDEX t_k ON ci.t (k);
	CREATE INDEX t_gk ON ci.t (g, k);
	CREATE TABLE ci.big (id int PRIMARY KEY, body text NOT NULL);
	ALTER TABLE ci.big ALTER COLUMN body SET STORAGE EXTERNAL;
	CREATE TABLE ci.bag (a int, b text);
	CREATE TABLE ci.dropped (id int PRIMARY KEY, v text);
	CREATE TABLE ci.remade (id int PRIMARY KEY, v text);
	INSERT INTO ci.t SELECT g, g % 97, g % 13, md5(g::text)
		FROM generate_series(1, $rows) g;
	INSERT INTO ci.big SELECT g, repeat(md5(g::text), 150)
		FROM generate_series(1, 60) g;
	INSERT INTO ci.bag SELECT g % 40, 'b' || (g % 40) FROM generate_series(1, 500) g;
	INSERT INTO ci.dropped SELECT g, 'd' || g FROM generate_series(1, 200) g;
	INSERT INTO ci.remade SELECT g, 'old' || g FROM generate_series(1, 200) g;
});
sql(q{SELECT dolt_commit('-Am', 'c1')});
my $c1 = sql(q{SELECT dolt_hashof('HEAD')});

sql(qq{
	UPDATE ci.t SET k = k + 1, v = 'c2' WHERE id % 10 = 0;
	DELETE FROM ci.t WHERE id % 50 = 1;
	UPDATE ci.big SET body = repeat(md5(id::text || 'c2'), 150) WHERE id <= 10;
	DELETE FROM ci.bag WHERE a = 3;
});
sql(q{SELECT dolt_commit('-am', 'c2')});
my $c2 = sql(q{SELECT dolt_hashof('HEAD')});

# a branch used and then deleted, its commit still mounted
sql(qq{
	SELECT dolt_branch('gone');
	SELECT dolt_checkout('gone');
	UPDATE ci.t SET v = 'gone' WHERE id BETWEEN 100 AND 900;
	INSERT INTO ci.big SELECT g, repeat(md5(g::text || 'gone'), 150)
		FROM generate_series(61, 80) g;
	SELECT dolt_commit('-am', 'on gone');
	SELECT prolly_mount('gone', 'ci', 'gm', NULL);
	SELECT dolt_checkout('main');
	SELECT dolt_branch('-D', 'gone');
});

# a branch used and then deleted that nothing reads: its commit is what a
# collection frees
sql(qq{
	SELECT dolt_branch('lost');
	SELECT dolt_checkout('lost');
	UPDATE ci.t SET v = 'lost' WHERE id <= 2000;
	UPDATE ci.big SET body = repeat(md5(id::text || 'lost'), 150);
	SELECT dolt_commit('-am', 'on lost');
	SELECT dolt_checkout('main');
	SELECT dolt_branch('-D', 'lost');
});

# a side branch, merged later
sql(qq{
	SELECT dolt_branch('side');
	SELECT dolt_checkout('side');
	UPDATE ci.t SET v = 'side' WHERE id % 10 = 5;
	INSERT INTO ci.big SELECT g, repeat(md5(g::text || 'side'), 150)
		FROM generate_series(81, 90) g;
	SELECT dolt_commit('-am', 'on side');
	SELECT dolt_checkout('main');
});

# a table dropped, and one dropped and made again
sql(qq{
	DROP TABLE ci.dropped;
	DROP TABLE ci.remade;
	CREATE TABLE ci.remade (id int PRIMARY KEY, w int NOT NULL);
	INSERT INTO ci.remade SELECT g, g * 3 FROM generate_series(1, 300) g;
	UPDATE ci.t SET g = g + 100 WHERE id % 7 = 0;
});
sql(q{SELECT dolt_commit('-Am', 'c3')});
my $c3 = sql(q{SELECT dolt_hashof('HEAD')});
sql(q{SELECT dolt_merge('side')});
my $c4 = sql(q{SELECT dolt_hashof('HEAD')});
isnt($c4, $c3, 'the merge made a commit');

# a kept detached checkout; mounts at an old commit and of the history; a
# branch the queries reset; dirty working sets on main and side, side's
# with a value out of line that only its page names (main's working set
# is folded by the queries' diff of it, which names its values)
sql(qq{
	SELECT dolt_checkout('$c2');
	SELECT dolt_checkout('main');
	SELECT prolly_mount('$c1', 'ci', 'm1', NULL);
	SELECT prolly_mount('HEAD', 'ci', 'hm', NULL, false, true);
	SELECT dolt_branch('scratch');
	UPDATE ci.t SET v = 'dirty' WHERE id <= 40;
	DELETE FROM ci.t WHERE id BETWEEN 2000 AND 2100;
	INSERT INTO ci.big VALUES (1000, repeat('dirty', 2000));
	SELECT dolt_checkout('side');
	UPDATE ci.t SET v = 'side dirty' WHERE id BETWEEN 500 AND 520;
	INSERT INTO ci.big SELECT 2000, string_agg(md5(g::text || 'side'), '')
		FROM generate_series(1, 300) g;
	SELECT dolt_checkout('main');
});
# A branch nothing else is made from, with an insert-only table whose
# pages only its commits' snapshots share, and six commits on it: the
# first ones leave prolly.kept_commits' window, their snapshots let go of.
# u2, of the same shape, is first used later by a branch the sweep must
# then stop counting.  (No query checks out a commit of solo: a checkout
# would share these pages.)
sql(q{
	SELECT dolt_branch('solo');
	SELECT dolt_checkout('solo');
	CREATE TABLE ci.u (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	CREATE TABLE ci.u2 (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO ci.u SELECT g, repeat('u', 100) FROM generate_series(1, 3000) g;
	INSERT INTO ci.u2 SELECT g, repeat('w', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 's1');
});
my $s1 = sql(q{SELECT dolt_checkout('solo'); SELECT dolt_hashof('HEAD')});
$s1 = (split /\n/, $s1)[-1];
for my $i (2 .. 6)
{
	sql(qq{
		SELECT dolt_checkout('solo');
		INSERT INTO ci.u SELECT g, 's$i' FROM generate_series($i * 10000, $i * 10000 + 19) g;
		SELECT dolt_commit('-am', 's$i');
	});
}

# On main, uncommitted: two tables extended in turn, so their blocks
# alternate in the page store (one dropped before the sweep, which then
# moves the other's down); and a table whose prune a crash will stop.
sql(q{
	CREATE TABLE ci.keep (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
	CREATE TABLE ci.gone_t (a int, b text) WITH (fillfactor = 10);
	CREATE TABLE ci.leak (a int8 PRIMARY KEY, b int8);
	INSERT INTO ci.leak SELECT g, g FROM generate_series(1, 3000) g;
	DO $$
	BEGIN
		FOR i IN 1 .. 150 LOOP
			INSERT INTO ci.keep SELECT g, repeat('k', 300)
				FROM generate_series(i * 10, i * 10 + 9) g;
			INSERT INTO ci.gone_t SELECT g, repeat('g', 300)
				FROM generate_series(i * 10, i * 10 + 9) g;
		END LOOP;
	END $$;
	SELECT dolt_branch('churn');
});

cmp_ok(sql(q{SELECT count(*) FROM pg_branch WHERE brname LIKE 'detached/%'}),
	'>=', 1, 'a detached checkout is kept');

# The queries: [name, select], each digested independent of row order.
# Those after a checkout or reset read where it left the session.
my @q = (
	[ point => 'SELECT * FROM ci.t WHERE id = 777' ],
	[ range => 'SELECT * FROM ci.t WHERE id BETWEEN 100 AND 400' ],
	[ full => 'SELECT * FROM ci.t' ],
	[ toast => 'SELECT id, md5(body), length(body) FROM ci.big' ],
	[ keyless => 'SELECT * FROM ci.bag' ],
	[ remade => 'SELECT * FROM ci.remade' ],
	[ keep => 'SELECT * FROM ci.keep' ],
	[ leak => 'SELECT * FROM ci.leak' ],
	[ set => 'SET enable_seqscan = off' ],
	[ set => 'SET enable_bitmapscan = off' ],
	[ index => 'SELECT * FROM ci.t WHERE k = 51' ],
	[ composite => 'SELECT * FROM ci.t WHERE g = 3 AND k < 50' ],
	[ index_only => 'SELECT g, k FROM ci.t WHERE g = 105' ],
	[ set => 'RESET enable_seqscan' ],
	[ set => 'RESET enable_bitmapscan' ],
	[ at_c1 => "SELECT * FROM dolt_at('$c1', NULL::ci.t) WHERE id BETWEEN 10 AND 600" ],
	[ at_c2_big => "SELECT id, md5(body) FROM dolt_at('$c2', NULL::ci.big)" ],
	[ at_side => "SELECT * FROM dolt_at('side', NULL::ci.t) WHERE id % 10 = 5" ],
	[ mount_t => 'SELECT * FROM m1.t WHERE id BETWEEN 50 AND 250' ],
	[ mount_big => 'SELECT id, md5(body) FROM m1.big' ],
	[ mount_dropped => 'SELECT * FROM m1.dropped' ],
	[ mount_remade => 'SELECT * FROM m1.remade' ],
	[ mount_gone => 'SELECT * FROM gm.t WHERE v = \'gone\'' ],
	[ mount_gone_big => 'SELECT id, md5(body) FROM gm.big WHERE id > 60' ],
	[ history_row => 'SELECT id, k, v, commit_hash FROM hm.t WHERE id = 10' ],
	[ history_c2 => "SELECT id, v FROM hm.t WHERE commit_hash = '$c2' AND id < 300" ],
	[ history_big => 'SELECT id, md5(body), commit_hash FROM hm.big WHERE id <= 12' ],
	[ diff_c1_c2 => "SELECT * FROM dolt_diff('$c1', '$c2', NULL::ci.t)" ],
	[ diff_working => "SELECT * FROM dolt_diff('HEAD', 'WORKING', NULL::ci.t)" ],
	[ diff_big => "SELECT diff_type, md5(from_row::text), md5(to_row::text) FROM dolt_diff('$c2', 'side', NULL::ci.big)" ],
	[ log_main => "SELECT commit_hash, message FROM dolt_log('main')" ],
	[ log_side => "SELECT commit_hash, message FROM dolt_log('side')" ],
	[ checkout => "SELECT dolt_checkout('$c2')" ],
	[ kept_t => 'SELECT * FROM ci.t' ],
	[ kept_big => 'SELECT id, md5(body) FROM ci.big' ],
	[ checkout => "SELECT dolt_checkout('side')" ],
	[ side_t => 'SELECT * FROM ci.t' ],
	[ side_big => 'SELECT id, md5(body) FROM ci.big' ],
	[ checkout => "SELECT dolt_checkout('scratch')" ],
	[ change => "UPDATE ci.t SET v = 'scratch' WHERE id <= 100" ],
	# (c2's ci.remade is the table before it was dropped and made again,
	# another relation: dropped here for the reset to make it)
	[ change => 'DROP TABLE ci.remade' ],
	[ reset => "SELECT dolt_reset('--hard', '$c2')" ],
	[ reset_t => 'SELECT * FROM ci.t' ],
	[ reset_big => 'SELECT id, md5(body) FROM ci.big' ],
	[ checkout => "SELECT dolt_checkout('solo')" ],
	[ solo_u => 'SELECT * FROM ci.u' ],
	[ solo_u2 => 'SELECT * FROM ci.u2' ],
	[ unkept_u => "SELECT * FROM dolt_at('$s1', NULL::ci.u)" ],
	[ checkout => "SELECT dolt_checkout('main')" ],
	[ back_t => 'SELECT * FROM ci.t WHERE id <= 1000' ],);

my $script = '';
for my $e (@q)
{
	my ($name, $select) = @$e;
	if ($name =~ /^(set|checkout|change|reset)$/)
	{
		$script .= "$select;\n";
		next;
	}
	$script .=
	  "SELECT 'q:$name|' || count(*) || '|' || coalesce(md5(string_agg(r::text, E'\\n' ORDER BY r::text)), '') FROM ($select) r;\n";
}

# Each query's digest, read in one session; a query that fails has none,
# and the session goes on to the next.
sub digests
{
	my ($out, $err) = ('', '');
	$node->psql(
		'postgres', $script,
		stdout => \$out,
		stderr => \$err,
		on_error_stop => 0);
	diag($err) if $err ne '';
	my %out;
	for my $line (split /\n/, $out)
	{
		$out{$1} = $2 if $line =~ /^q:(\w+)\|(.*)$/;
	}
	return \%out;
}

my $base = digests();
my @names = map { $_->[0] } grep { $_->[0] !~ /^(set|checkout|change|reset)$/ } @q;
is(scalar(keys %$base), scalar(@names), 'every query digested');
for my $name (qw(point range full toast keyless remade index composite
	index_only at_c1 mount_t mount_dropped mount_gone history_row
	diff_c1_c2 diff_working log_main kept_t side_t reset_t keep leak solo_u
	solo_u2 unkept_u))
{
	unlike($base->{$name}, qr/^0\|/, "query $name reads rows");
}
is_deeply(digests(), $base, 'the queries read the same twice');

# Every digest as it was before any cleanup; the queries that changed
# named.  First the blocks freed are written again, by a table on a branch
# no query reads, and every buffer is evicted: a read of a freed block
# then finds that table's page, not a cached copy of what was there.
sub same_after
{
	my ($cleanup) = @_;
	sql(q{
		SELECT dolt_checkout('churn');
		DROP TABLE IF EXISTS ci.churn;
		CREATE TABLE ci.churn (a int, b text);
		INSERT INTO ci.churn SELECT g, repeat(md5(g::text), 8) FROM generate_series(1, 6000) g;
		CHECKPOINT;
		SELECT count(*) FROM pg_buffercache_evict_all();
	});
	my $now = digests();
	my @changed = grep { ($now->{$_} // 'missing') ne $base->{$_} } @names;
	is(join(', ', @changed), '', "every query reads the same after $cleanup");
}

# pg_stat_prolly's counter, every session's counts flushed (each sql() is
# a session, which flushes as it ends)
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

my $visible_u = q{SELECT relallvisible || '/' || relpages FROM pg_class
	WHERE oid = 'ci.u'::regclass};

# VACUUM on main, and on solo's insert-only table: kept-only pages are
# copied (set all-visible) only with prolly.vacuum_copy_kept_pages on.
# (An aggressive VACUUM, FREEZE or DISABLE_PAGE_SKIPPING, copies them
# either way, so those come after.)
# u's map, on solo: its blocks' places in the page store
my $u_map = q{SELECT dolt_checkout('solo');
	SELECT string_agg(phys::text, ',' ORDER BY blkno)
	FROM test_bmap_map(pg_relation_filenode('ci.u'))};
sub last_line { return (split /\n/, $_[0])[-1]; }
for my $copy ('off', 'on')
{
	my $setting = "SET prolly.vacuum_copy_kept_pages = $copy";
	sql("$setting; VACUUM");
	my $before = last_line(sql($u_map));
	my $copies = counted('page_copies_shared');
	my ($all, $pages) = split m{/},
	  last_line(sql(qq{SELECT dolt_checkout('solo'); $setting; VACUUM ci.u; $visible_u}));
	my @was = split /,/, $before;
	my @now = split /,/, last_line(sql($u_map));
	my $moved = grep { $was[$_] ne $now[$_] } 0 .. $#was;
	if ($copy eq 'off')
	{
		is($moved, 0, 'VACUUM with copying off copies no kept-only page');
		is($all, 0, 'and sets none all-visible');
	}
	else
	{
		is($moved, scalar(@was), 'VACUUM with copying on copies every kept-only page');
		is($all, $pages, 'and sets every page all-visible');
		cmp_ok(counted('page_copies_shared') - $copies, '>=', $moved,
			'pg_stat_prolly counts the copies');
	}
	same_after("VACUUM, copying kept pages $copy");
}
for my $vacuum (
	'VACUUM (DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON)',
	'VACUUM (FREEZE)',
	'SET prolly.vacuum_copy_kept_pages = on; VACUUM (FREEZE)',
	'VACUUM (ANALYZE)')
{
	sql($vacuum);
	same_after($vacuum);
}

# the side branch vacuumed too, from a session there
sql(q{SELECT dolt_checkout('side'); VACUUM (FREEZE); SELECT dolt_checkout('main')});
same_after('VACUUM on side');

# a branch deleted and a table dropped queue their storage for release:
# autovacuum drains the queue (its automatic collections off meanwhile:
# the collections are their own cleanup, below), and every worker is done
# before the next cleanup
sql(q{
	SELECT dolt_branch('doomed', 'side');
	SELECT dolt_checkout('doomed');
	UPDATE ci.t SET v = 'doomed' WHERE id <= 500;
	SELECT dolt_commit('-am', 'doomed');
	SELECT dolt_checkout('main');
	SELECT dolt_branch('-D', 'doomed');
	CREATE TABLE public.scrap AS SELECT g FROM generate_series(1, 20000) g;
	DROP TABLE public.scrap;
});
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0, 'storage is queued for release');
my $released = counted('bmap_release_blocks_freed');
$node->append_conf('postgresql.conf', "autovacuum = on\nprolly.gc_journal_threshold = -1");
$node->reload;
ok( $node->poll_query_until(
		'postgres', q{SELECT test_bmap_release_pending() = 0}),
	'autovacuum drains the release queue');
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly.gc_journal_threshold = 128");
$node->reload;
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
			WHERE backend_type = 'autovacuum worker'}),
	'and its workers are done');
cmp_ok(counted('bmap_release_blocks_freed') - $released, '>', 0,
	'the drain freed blocks');
same_after('autovacuum drained the release queue');

# the collections: the full one before a default one, so that it collects
# a store no default collection has settled first
for my $gc ("SELECT dolt_gc('--full')", 'SELECT dolt_gc()')
{
	my $freed = counted('gc_chunks_freed');
	sql($gc);
	note "$gc: freed " . (counted('gc_chunks_freed') - $freed) . ' chunks';
	cmp_ok(counted('gc_chunks_freed') - $freed, '>', 0, "$gc frees chunks")
	  if $gc =~ /full/;
	same_after($gc);
}

# What the sweep must act on:
# - u2 first used by a branch, then the branch deleted: the sweep lowers
#   u2's shared_below, and VACUUM, which left the shared pages, copies them
# - stuck counts: 300 copies of t's map, all but five dropped
# - gone_t dropped: keep's blocks above its holes are moved down
# - a prune stopped by a crash after letting go of its old pages: they are
#   allocated and nothing holds them
sql(q{
	SELECT dolt_branch('ub', 'solo');
	SELECT dolt_checkout('ub');
	SELECT count(*) FROM ci.u2;
});
my $visible_u2 = q{SELECT relallvisible || '/' || relpages FROM pg_class
	WHERE oid = 'ci.u2'::regclass};
my $out = sql(qq{SELECT dolt_checkout('solo');
	SET prolly.vacuum_copy_kept_pages = on; VACUUM ci.u2; $visible_u2});
like($out, qr/\n0\/\d+$/, 'VACUUM leaves the pages ub shares');
sql(q{SELECT dolt_branch('-D', 'ub')});

sql('DROP TABLE ci.gone_t');

my $leaked = 0;
if ($ENV{enable_injection_points} eq 'yes')
{
	sql('CREATE EXTENSION injection_points');
	my $rel = sql(q{SELECT pg_relation_filenode('ci.leak')});
	my ($lineage, $e1) = split /\|/,
	  sql(qq{SELECT lineage, epoch FROM test_bmap_fold_begin($rel)});
	sql(q{UPDATE ci.leak SET b = b + 1 WHERE a % 100 = 0});
	my $e2 = sql(qq{SELECT epoch FROM test_bmap_fold_begin($rel)});
	$leaked = sql(qq{SELECT count(DISTINCT old_block)
		FROM test_bmap_fold_changes($rel, $lineage, $e1) WHERE kind = 'copy'});
	# (the update is read: the base digest is of the rows before it)
	$base->{leak} = digests()->{leak};
	my $p = $node->background_psql('postgres', on_error_stop => 0);
	$p->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-pruned', 'wait')});
	$p->query_until(qr/started/, qq{
		\\echo started
		SELECT test_bmap_fold_prune($rel, $lineage, $e2);
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'bmap-pruned'}) or die 'the prune did not stop';
	# (the prune's records flushed before the crash, which keeps only
	# the WAL written out)
	my $pruned = sql('SELECT pg_current_wal_insert_lsn()');
	sql(q{SELECT pg_logical_emit_message(false, 'flush', '', true)});
	is(sql(qq{SELECT pg_current_wal_flush_lsn() >= '$pruned'}), 't',
		'the prune\'s records are flushed');
	$node->stop('immediate');
	eval { $p->quit; };
	$node->start;
	cmp_ok($leaked, '>', 0, 'a crashed prune leaves blocks nothing holds');
}

# (after the crash: a copy test_bmap_unlink() drops is unlinked only by
# a checkpoint, and one a crash came first to would still count)
my $t_rel = sql(q{SELECT pg_relation_filenode('ci.t')});
sql(qq{CREATE TABLE public.copies AS
	SELECT test_bmap_share($t_rel) AS copy FROM generate_series(1, 300)});
sql(q{WITH d AS (DELETE FROM public.copies
		WHERE ctid NOT IN (SELECT ctid FROM public.copies LIMIT 5) RETURNING copy)
	SELECT count(test_bmap_unlink(copy)) FROM d});
cmp_ok(sql(qq{SELECT count(*) FROM test_bmap_tree($t_rel) WHERE refs = 255}), '>', 0,
	'the copies leave stuck counts');
sql('CHECKPOINT');
my ($freed, $recounted, $truncated, $moved) =
  split /\|/, sql(q{SELECT * FROM prolly_page_sweep()});
note "sweep: freed $freed, recounted $recounted, truncated $truncated, moved $moved";
if ($leaked)
{
	cmp_ok($freed, '>=', $leaked, 'the sweep frees the blocks nothing holds');
}
cmp_ok($recounted, '>', 0, 'the sweep recounts the stuck counts');
cmp_ok($moved, '>', 0, 'the sweep moves blocks down');
$out = sql(qq{SELECT dolt_checkout('solo');
	SET prolly.vacuum_copy_kept_pages = on; VACUUM ci.u2; $visible_u2});
my ($u2all, $u2pages) = $out =~ /(\d+)\/(\d+)$/;
is($u2all, $u2pages, 'the sweep lowered shared_below: VACUUM copies the pages ub shared');
sql(q{SELECT count(test_bmap_unlink(copy)) FROM public.copies; DROP TABLE public.copies});
same_after('the page sweep');

sql('CHECKPOINT');
same_after('CHECKPOINT');

# every cleanup again, now that the queue is drained and nothing is left
# to free but what they free themselves
sql(q{VACUUM (FREEZE); SELECT dolt_gc('--full'); SELECT * FROM prolly_page_sweep(); CHECKPOINT});
same_after('a second round of cleanups');

# cold: after a restart, and after a crash
$node->restart;
same_after('a restart');
sql(q{SELECT dolt_gc('--full'); SELECT * FROM prolly_page_sweep()});
$node->stop('immediate');
$node->start;
same_after('a crash after cleanup');
sql(q{SELECT * FROM prolly_page_sweep(); CHECKPOINT});
same_after('a sweep after recovery');

# the folds still match their trees, and every index its table, on main
# and on side
is(sql(q{SELECT count(*) FILTER (WHERE NOT matches) || '/' || (count(*) > 0)::text
	FROM dolt_verify_folds()}), '0/true', 'every fold matches its tree');
my $indexes = q{SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
	WHERE i.indrelid IN (SELECT oid FROM pg_class
		WHERE relnamespace = 'ci'::regnamespace)};
is(sql($indexes), 't', 'every index passes amcheck on main');
like(sql(qq{SELECT dolt_checkout('side'); $indexes}), qr/\nt$/,
	'every index passes amcheck on side');

# no error or warning anywhere in the log (the background workers a
# shutdown terminates aside)
my $log = slurp_file($node->logfile, $log_start);
my @bad = grep { /\b(ERROR|WARNING|PANIC|FATAL):/ && !/due to administrator command/ }
  split /\n/, $log;
is(join("\n", @bad), '', 'the server log has no errors or warnings');

$node->stop;
done_testing();
