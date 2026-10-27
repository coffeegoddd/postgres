# Copyright (c) 2026, PostgreSQL Global Development Group

# Garbage collection of the chunk store, online: a writer running at the
# swap that then names a collected chunk is refused (40001), what it
# appended before the swap is kept; the retire waits for the swap's
# readers; a standby makes the same files, and its queries running at the
# swap are cancelled when the files are retired; a crash at each step
# leaves the store whole, and the next collection finishes the work.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('gc');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE gcdb');
$node->safe_psql('gcdb', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('gc_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
max_standby_streaming_delay = 50ms
hot_standby_feedback = off
});
$standby->start;

sub sql { return $node->safe_psql('gcdb', $_[0]); }

# the store's files, each with its md5
my $files = q{SELECT string_agg(f || ' ' || md5(pg_read_binary_file(d || '/' || f)), E'\n' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'gcdb') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_%' AND f NOT LIKE '%.tmp'
		AND f <> 'prolly_index'};	# (each server's own cache)
my $on_disk = q{SELECT string_agg(f, ' ' ORDER BY f)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'gcdb') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_journal%' OR f LIKE 'prolly\_table\_%'};
my $named = q{SELECT string_agg(name, ' ' ORDER BY name) FROM
	(SELECT name FROM test_chunk_segments() UNION ALL
	 SELECT name FROM test_chunk_table_files()) n};
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

# A root reaching 20 chunks through a node, and 20 chunks nothing reaches.
my $live = sql(q{SELECT array_agg(test_chunk_put(convert_to('live ' || g, 'UTF8')))
	FROM generate_series(1, 20) g});
my $root = sql("SELECT test_chunk_put_node(1, '$live')");
sql("SELECT test_chunk_move_root(NULL, '$root')");
my $dead = sql(q{SELECT test_chunk_put('dead')});
sql(q{SELECT test_chunk_put(convert_to('dead ' || g, 'UTF8')) FROM generate_series(1, 19) g});
my $reads = q{SELECT count(*) FROM generate_series(1, 20) g
	WHERE test_chunk_has(test_content_hash(convert_to('live ' || g, 'UTF8')))};

is(sql('SELECT kept || \' \' || freed FROM test_chunk_gc(false)'), '21 20',
	'the live chunks kept, the dead freed');
is(sql($reads), '20', 'the live ones read');
is(has($dead), 'f', 'a dead one is gone');
is(sql($on_disk), sql($named), 'only the files the manifest names are left');

# A collection stopped once it has waited for the transactions running as
# it began (those it waits for before marking what is new, and which keep
# what they name): a writer begun then runs at its swap.
sub collect_after_running
{
	my $c = $node->background_psql('gcdb');
	$c->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-gc-running-waited', 'wait')});
	$c->query_until(qr/started/, q{
		\echo started
		SELECT test_chunk_gc(false) IS NOT NULL;
		\echo done
	});
	$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-gc-running-waited'})
	  or die 'the collection did not stop';
	return $c;
}

# A writer running at the swap, begun after the collection waited for
# those running before: what it names that was collected is refused; what
# it appended before the swap is kept.
sql("SELECT test_chunk_put('orphan')");
my $orphan = sql(q{SELECT test_content_hash('orphan')});
my $w = $node->background_psql('gcdb', on_error_stop => 0);
my $g = collect_after_running();
$w->query_safe(q{BEGIN; SELECT test_chunk_root() IS NOT NULL});
my $early = $w->query_safe(qq{SELECT test_chunk_put('written before the swap')});
$w->query_safe(qq{SELECT test_chunk_put(test_noise('w', 1048576))});
sql(q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the writer';
is(has($orphan), 'f', 'a chunk nothing reached is collected');
$w->query(qq{SELECT test_chunk_put_node(1, ARRAY['$orphan']); COMMIT});
like($w->{stderr}, qr/is not in the chunk store/,
	'a writer naming it after the swap, as a tree\'s child, is refused');
$w->{stderr} = '';
$g->query_until(qr/done/, '');
$g->quit;
is(has($early), 't', 'what a writer appended between the seal and the swap is kept');

# Bytes of a chunk that spell a collected chunk's hash but aren't a
# reference it declares name nothing (a collection keeps nothing for
# them): the writer commits.
sql("SELECT test_chunk_put('orphan 2')");
my $orphan2 = sql(q{SELECT test_content_hash('orphan 2')});
$g = collect_after_running();
$w->query_safe(q{BEGIN; SELECT test_chunk_root() IS NOT NULL});
sql(q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the writer';
$w->query(qq{SELECT test_chunk_put('\\x0120'::bytea || '\\x14'::bytea || decode('$orphan2', 'hex')); COMMIT});
is($w->{stderr}, '', 'a writer whose chunk\'s bytes spell a collected hash commits');
$w->{stderr} = '';
$g->query_until(qr/done/, '');
$g->quit;

# What a stored chunk a writer read names is trusted to be stored, so a
# chunk naming it needs no index probe: but not once a swap lands after
# the read, which may drop both.  A node and its child nothing reaches,
# the node read (from the files, or the cache) by a writer with a chunk
# pending; then a collection's swap; then the writer names the child.
my $case = 0;
for my $from ('files', 'cache')
{
	$case++;
	sql("SELECT test_chunk_put('trusted orphan $case')");
	my $child = sql(qq{SELECT test_content_hash('trusted orphan $case')});
	my $node_hash = sql(qq{SELECT test_chunk_put_node(1, ARRAY['$child'])});
	if ($from eq 'cache')
	{
		# (read twice before: kept by the cache, the third read its)
		$w->query_safe(qq{SELECT test_chunk_get('$node_hash') IS NOT NULL}) for 1 .. 2;
	}
	$g = collect_after_running();
	$w->query_safe(qq{BEGIN; SELECT test_chunk_put('pending $case');
		SELECT test_chunk_get('$node_hash') IS NOT NULL});
	sql(q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
	$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the writer';
	$w->query(qq{SELECT test_chunk_put_node(1, ARRAY['$child']); COMMIT});
	# (refused by the probe the trust skipped before the swap)
	like($w->{stderr}, qr/which it names, is not in the chunk store/,
		"a writer naming a child it trusted (its node read from the $from) after a swap is refused");
	$w->{stderr} = '';
	$g->query_until(qr/done/, '');
	$g->quit;
}

# A root moved to (an older one, say), that a swap since the transaction
# began dropped, is refused, though the transaction puts nothing.
sql("SELECT test_chunk_put('orphan root')");
my $orphan3 = sql(q{SELECT test_content_hash('orphan root')});
$g = collect_after_running();
$w->query_safe(q{BEGIN; SELECT test_chunk_root() IS NOT NULL});
sql(q{SELECT injection_points_wakeup('prolly-gc-running-waited')});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the writer';
$w->query(qq{SELECT test_chunk_move_root('$root', '$orphan3'); COMMIT});
like($w->{stderr}, qr/could not move the chunk store's root to [0-9a-f]+: it (is not in the chunk store|was collected while the transaction ran)/,
	'a root move to a chunk the swap dropped is refused');
$w->{stderr} = '';
$g->query_until(qr/done/, '');
$g->quit;
is(sql('SELECT test_chunk_root()'), $root, 'and the root is where it was');

# A second collection while one runs is refused, not left to wait (the
# first waits for every transaction running at its swap).
$g = $node->background_psql('gcdb');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-sealed', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo done
});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-sealed'}) or die 'the collection did not stop';
my ($ret, $out, $err) = $node->psql('gcdb', 'SELECT test_chunk_gc(false)',
	timeout => 30);
like($err, qr/another collection of this database's chunk store is running/,
	'a second collection is refused while one runs');
sql(q{SELECT injection_points_wakeup('prolly-gc-sealed')});
$g->query_until(qr/done/, '');
$g->quit;

# A collection that fails in a subtransaction is abandoned with it: the
# transaction may collect again.
my $sub = sql(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-sealed', 'error');
	DO $$
	BEGIN
		BEGIN
			PERFORM test_chunk_gc(false);
		EXCEPTION WHEN OTHERS THEN
			RAISE NOTICE 'failed';
		END;
		PERFORM injection_points_detach('prolly-gc-sealed');
		PERFORM test_chunk_gc(false);
	END $$;
	SELECT 'collected'});
like($sub, qr/collected/, 'a collection failed in a subtransaction leaves the transaction free to collect');
is(sql($reads), '20', 'and the live chunks read');

# A writer that started before the swap and names nothing collected
# commits.
$w->query_safe(qq{BEGIN; SELECT test_chunk_has('$root')});
$g = $node->background_psql('gcdb');
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo done
});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the writer';
my $after = $w->query_safe(qq{SELECT test_chunk_put_node(1, ARRAY['$root']); COMMIT});
is($w->{stderr}, '', 'a writer naming only live chunks commits across the swap');
$g->query_until(qr/done/, '');
$g->quit;
$w->quit;

# A transaction adding a table file (a transfer's) that began before a
# swap is refused: at once while the collection runs, rather than waiting
# for the table file writers' lock the collection holds while it waits
# for the transaction.
my $t = $node->background_psql('gcdb', on_error_stop => 0);
$t->query_safe(qq{BEGIN; SELECT test_chunk_has('$root')});
$g = $node->background_psql('gcdb');
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo done
});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the transfer';
$t->query(q{SELECT test_chunk_table_file(ARRAY['received'::bytea]); COMMIT});
like($t->{stderr}, qr/the chunk store is being collected/,
	'a table file added by a transaction older than the swap is refused, at once');
$g->query_until(qr/done/, '');
$g->quit;
$t->quit;

# The standby's files are the primary's; a standby query running at the
# swap is cancelled when the files are retired.
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('gcdb', $files), sql($files),
	'a standby\'s files after collections are the primary\'s, byte for byte');
is($standby->safe_psql('gcdb', $reads), '20', 'and it reads them');
my $log_location = -s $standby->logfile;
my $s = $standby->background_psql('gcdb', on_error_stop => 0);
$s->query_safe(qq{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT test_chunk_has('$root')});
sql(q{SELECT test_chunk_put('more dead')});
sql('SELECT test_chunk_gc(false)');
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$standby->wait_for_log(qr/conflict with recovery/, $log_location);
ok($standby->poll_query_until('gcdb',
	q{SELECT confl_chunk_store > 0 FROM pg_stat_database_conflicts WHERE datname = 'gcdb'}),
	'a standby query running at the swap is cancelled at the retire');
eval { $s->quit; };	# (the conflict ended its session)
is($standby->safe_psql('gcdb', $reads), '20', 'and a new one reads the new files');

# A crash at each step of a collection leaves the store whole; the end of
# recovery removes what it left, before anyone reads the store.
for my $point ('prolly-gc-sealed', 'prolly-gc-copied', 'prolly-gc-window',
	'prolly-gc-swapped', 'prolly-gc-retire')
{
	sql(q{SELECT test_chunk_put(convert_to('dead at ' || clock_timestamp(), 'UTF8'))});
	my $c = $node->background_psql('gcdb', on_error_stop => 0);
	$c->query_safe(qq{SELECT injection_points_set_local();
		SELECT injection_points_attach('$point', 'wait')});
	$c->query_until(qr/started/, q{
		\echo started
		SELECT test_chunk_gc(false);
	});
	$node->poll_query_until('gcdb', qq{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = '$point'}) or die "the collection did not stop at $point";
	$node->stop('immediate');
	eval { $c->quit; };
	$node->start;
	is(sql($on_disk), sql($named), "a crash at $point: recovery leaves only what is named");
	is(sql(q{SELECT count(*) FROM (SELECT 'base/' || oid AS d FROM pg_database
		WHERE datname = 'gcdb') db, pg_ls_dir(d) f
		WHERE f = 'prolly_manifest.tmp'}), '0',
		"a crash at $point: and no manifest written aside");
	is(sql($reads), '20', "a crash at $point: the live chunks read");
	is(sql('SELECT test_chunk_root()'), $root, "a crash at $point: the root");
	sql('SELECT test_chunk_gc(false)');
	is(sql($on_disk), sql($named), "a crash at $point: the next collection leaves only what is named");
	is(sql($reads), '20', "a crash at $point: and the live chunks read after it");
}

# A full collection keeps a mostly live settled file whole, garbage and
# all: a chunk of its garbage stays found, though another copy of it (in
# a file rewritten) is garbage too, so a writer naming it across the swap
# commits.
# (A chunk named by one put during a collection, after its seal, is
# copied with the live ones; once that one is collected, nothing names it.)
my $x = sql(q{SELECT test_chunk_put('kept x')});
my $kl = sql(q{SELECT array_agg(test_chunk_put(convert_to('kept live ' || g, 'UTF8')))
	FROM generate_series(1, 10) g});
my $root_k = sql(qq{SELECT test_chunk_put_node(1, '$kl'::text[])});
sql("SELECT test_chunk_move_root('$root', '$root_k')");
$g = $node->background_psql('gcdb');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-sealed', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo done
});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-sealed'}) or die 'the collection did not stop';
sql(qq{SELECT test_chunk_put_node(1, ARRAY['$x'])});
sql(q{SELECT injection_points_wakeup('prolly-gc-sealed')});
$g->query_until(qr/done/, '');
$g->quit;
sql('SELECT test_chunk_gc(false)');
sql(q{SELECT test_chunk_table_file(ARRAY['kept x'::bytea])});
my $k = $node->background_psql('gcdb', on_error_stop => 0);
$k->query_safe(qq{BEGIN; SELECT test_chunk_has('$root_k')});
$g = $node->background_psql('gcdb');
$g->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_gc(true) IS NOT NULL;
	\echo done
});
$node->poll_query_until('gcdb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'}) or die 'the collection did not wait for the writer';
$k->query(qq{SELECT test_chunk_put_node(1, ARRAY['$x']); COMMIT});
is($k->{stderr}, '', 'a writer naming garbage a kept file holds commits across the swap');
$g->query_until(qr/done/, '');
$g->quit;
$k->quit;
is(has($x), 't', 'and the chunk is still there');

$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('gcdb', $files), sql($files),
	'the standby\'s files are the primary\'s after it all');

# A crash as a store's first table file is written, before any manifest
# (and with no journal): recovery removes the file no manifest names, in
# a directory that holds it alone.
$node->safe_psql('postgres', 'CREATE DATABASE first_file');
$node->safe_psql('first_file',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
my $ff = $node->data_dir . '/base/'
  . $node->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = 'first_file'});
my $store_files = sub {
	opendir(my $dh, $ff) or die "opendir $ff: $!";
	my @f = sort grep { /^prolly_/ } readdir($dh);
	closedir($dh);
	return join(' ', @f);
};
is($store_files->(), '', 'a new database has no store files');
my $writer = $node->background_psql('first_file');
$writer->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-table-file-written', 'wait')});
$writer->query_until(qr/started/, q{
	\echo started
	SELECT test_chunk_table_file(ARRAY['first file'::bytea]);
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-table-file-written'})
  or die 'the writer did not wait';
like($store_files->(), qr/^prolly_table_\S+$/, 'the file written, alone');
$standby->stop;
$node->stop('immediate');
$writer->{run}->finish;
$node->start;
is($store_files->(), '', 'recovery removes it');

$node->stop;
done_testing();
