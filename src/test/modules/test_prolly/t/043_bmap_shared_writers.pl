# Copyright (c) 2026, PostgreSQL Global Development Group

# Many sessions write a block-mapped table and its copy at once, through
# the same shared pages: pgbench clients update rows (HOT and not), delete
# and reinsert them, and VACUUM both sides, all while the other side does
# the same.  Each side's first change to a shared page copies it, racing
# the other side's.  After, each side has exactly its own changes, none of
# the other's, through a sequential scan and its index alike; amcheck finds
# both sound; and so after a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('shared_writers');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
max_connections = 40
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $rows = 20000;
sql(qq{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION amcheck;
	CREATE TABLE t (k int PRIMARY KEY, side text, n int, pad text) WITH (fillfactor = 90);
	INSERT INTO t SELECT g, 'base', 0, repeat('p', 100) FROM generate_series(1, $rows) g;
	CREATE TABLE c (LIKE t INCLUDING ALL) WITH (fillfactor = 90);
	CREATE TABLE log_t (k int);
	CREATE TABLE log_c (k int);
});
sql(q{VACUUM (FREEZE) t});
sql(q{
	UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('t')),
		relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = 't'),
		relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = 't')
		WHERE relname = 'c';
	UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('t_pkey'))
		WHERE relname = 'c_pkey';
});
is(sql(q{SELECT count(*) FILTER (WHERE refs = 0) FROM test_bmap_map(pg_relation_filenode('c'))}),
	'0', 'every page of the copy is shared with the table');

# Each client changes only its own keys (k mod clients), so no two
# clients wait on one row; each transaction adds one to n.
my $clients = 8;
my %scripts;
for my $rel ('t', 'c')
{
	$scripts{"update_$rel"} = qq{
\\set k random(0, $rows / $clients - 1) * $clients + :client_id + 1
BEGIN;
UPDATE $rel SET side = '$rel', n = n + 1 WHERE k = :k;
INSERT INTO log_$rel VALUES (:k);
COMMIT;
};
	$scripts{"move_$rel"} = qq{
\\set k random(0, $rows / $clients - 1) * $clients + :client_id + 1
BEGIN;
WITH d AS (DELETE FROM $rel WHERE k = :k RETURNING *)
INSERT INTO $rel SELECT k, '$rel', n + 1, pad FROM d;
INSERT INTO log_$rel VALUES (:k);
COMMIT;
};
	$scripts{"vacuum_$rel"} = qq{VACUUM $rel;
};
}
my %weight = (update_t => 20, update_c => 20, move_t => 5, move_c => 5,
	vacuum_t => 1, vacuum_c => 1);
my @f;
for my $name (sort keys %scripts)
{
	push @f, '-f', "$name\@$weight{$name}";
}

my $bindir = $node->config_data('--bindir');
my $dir = PostgreSQL::Test::Utils::tempdir;
for my $name (sort keys %scripts)
{
	open my $fh, '>', "$dir/$name" or die;
	print $fh $scripts{$name};
	close $fh;
}
my ($stdout, $stderr);
my $ok = IPC::Run::run(
	[ 'pgbench', '-n', '-c', $clients, '-j', $clients, '-T', 15,
	  map({ /\@/ ? "$dir/$_" : $_ } @f),
	  '-h', $node->host, '-p', $node->port, 'postgres' ],
	'>', \$stdout, '2>', \$stderr);
ok($ok, 'pgbench ran both sides at once') or diag($stderr);
like($stdout, qr/number of failed transactions: 0 /, 'with no failed transactions');

# Transactions each script ran.
my %ran;
while ($stdout =~ /SQL script \d+: \S*\/(\w+)\n - weight: \d+ .*\n - (\d+) transactions/g)
{
	$ran{$1} = $2;
}
for my $rel ('t', 'c')
{
	cmp_ok($ran{"update_$rel"} // 0, '>', 100, "$rel was updated many times");
	cmp_ok($ran{"move_$rel"} // 0, '>', 20, "and its rows moved");
	cmp_ok($ran{"vacuum_$rel"} // 0, '>', 0, "and vacuumed");
}
cmp_ok(sql(q{SELECT count(*) FILTER (WHERE refs = 0) FROM test_bmap_map(pg_relation_filenode('c'))}),
	'>', 0, 'the copy\'s changed pages were copied');

# Sweep until a sweep frees nothing: the blocks freed.
sub sweep_all
{
	my $total = 0;
	for (1 .. 10)
	{
		my $freed = sql(q{SELECT freed FROM test_bmap_sweep()});
		last if $freed == 0;
		$total += $freed;
	}
	return $total;
}

sub check
{
	my ($what) = @_;
	for my $rel ('t', 'c')
	{
		my $other = $rel eq 't' ? 'c' : 't';
		my $changes = sql(qq{SELECT count(*) FROM log_$rel});
		is(sql(qq{SELECT count(*) FROM $rel r
			FULL JOIN (SELECT k, count(*) AS m FROM log_$rel GROUP BY k) l USING (k)
			WHERE r.n IS DISTINCT FROM coalesce(l.m, 0)}), '0',
			"$what: each of ${rel}'s rows has exactly the changes made to it");
		is(sql(qq{SELECT count(*) || ':' || sum(k) || ':' || sum(n) FROM $rel}),
			"$rows:" . ($rows * ($rows + 1) / 2) . ":$changes",
			"$what: $rel has every row once, and exactly its own $changes changes");
		is(sql(qq{SELECT count(*) FROM $rel WHERE side = '$other'}), '0',
			"$what: and none of $other\'s");
		is(sql(qq{SET enable_seqscan = off; SET enable_bitmapscan = off;
			SELECT count(*) || ':' || sum(k) || ':' || sum(n) FROM $rel WHERE k > 0}),
			"$rows:" . ($rows * ($rows + 1) / 2) . ":$changes",
			"$what: its index finds the same");
		is(sql(qq{SELECT count(*) FROM verify_heapam('$rel')}), '0',
			"$what: amcheck finds $rel\'s heap sound");
		is(sql(qq{SELECT bt_index_parent_check('${rel}_pkey', true, true)}), '',
			"$what: and its index");
	}
}

check('after the run');
$node->stop('immediate');
$node->start;
check('after a crash');
sql(q{VACUUM t; VACUUM c;});
check('after VACUUM');

# Both sides copying the same shared node of the block map's tree at
# once: each relation's path changes under its own header only, so one
# side may find a node shared that the other copies before it takes the
# counts' lock.  It is then the first side's alone, and is claimed rather
# than copied (it used to fail "is copied but held once"); the holders its
# children were given first, each in a record of its own here, are taken
# back, so once both sides are dropped nothing is left for a sweep.
SKIP:
{
	skip 'injection points not supported by this build', 8
	  unless $ENV{enable_injection_points} eq 'yes';
	sql(q{
		CREATE EXTENSION IF NOT EXISTS injection_points;
		CREATE TABLE u (k int PRIMARY KEY, n int, pad text);
		INSERT INTO u SELECT g, 0, repeat('u', 100) FROM generate_series(1, 20000) g;
		CREATE TABLE v (LIKE u INCLUDING ALL);
	});
	sql(q{VACUUM (FREEZE) u});
	sql(q{
		UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('u')),
			relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE relname = 'u'),
			relminmxid = (SELECT relminmxid FROM pg_class WHERE relname = 'u')
			WHERE relname = 'v';
		UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('u_pkey'))
			WHERE relname = 'v_pkey';
	});
	my $first = $node->background_psql('postgres');
	$first->query_safe(q{SELECT injection_points_set_local()});
	$first->query_safe(q{SELECT injection_points_attach('bmap-tree-copy-counts', 'wait')});
	$first->query_safe(q{SELECT injection_points_attach('bmap-tree-hold-first', 'notice')});
	$first->query_until(qr/^$/, "UPDATE u SET n = 1 WHERE k = 10000;\n");
	$node->wait_for_event('client backend', 'bmap-tree-copy-counts');
	sql(q{UPDATE v SET n = 2 WHERE k = 10000});
	# (detached before the wakeup: a path of more levels would wait again)
	sql(q{SELECT injection_points_detach('bmap-tree-copy-counts')});
	sql(q{SELECT injection_points_wakeup('bmap-tree-copy-counts')});
	my $done = eval {
		$first->query_safe(q{SELECT 1});
		1;
	};
	ok($done, 'the change held at the copy goes on, the node now its own')
	  or diag($@);
	is(sql(q{SELECT n FROM u WHERE k = 10000}), '1', 'and is made');
	is(sql(q{SELECT n FROM v WHERE k = 10000}), '2', 'and the other side keeps its own');
	is(sql(q{SELECT count(*) FILTER (WHERE n <> 0) FROM u}), '1', 'u has only its change');
	is(sql(q{SELECT count(*) FILTER (WHERE n <> 0) FROM v}), '1', 'and v only its own');
	is(sql(q{SELECT count(*) FROM verify_heapam('u')}) . sql(q{SELECT count(*) FROM verify_heapam('v')}),
		'00', 'amcheck finds both heaps sound');
	$first->quit;
	ok(!$node->log_contains(qr/is copied but held once/), 'no copy failed');

	# (what earlier steps left for a sweep, first)
	sweep_all();
	sql(q{DROP TABLE u, v});
	sql(q{SELECT * FROM test_bmap_release_drain()});
	is(sweep_all(), 0, 'dropping both sides frees every block: no child is held once too often');
}

# A lookup reads a map's tree under no lock of its header, a node at a
# time.  One holding a pointer to a node its map shares, read before its
# map copied the node, must not follow it into the other map once that
# map, the node now its own, changes it: the node is moved to a new block
# rather than changed in place, and the lookup, finding the old block no
# longer the node it named, begins again from the root.
SKIP:
{
	skip 'injection points not supported by this build', 4
	  unless $ENV{enable_injection_points} eq 'yes';
	# (a copy shared of the source, and storage made of a root captured of
	# it, the root then let go of: either shares the source's nodes)
	for my $how ('share', 'root')
	{
		sql(qq{
			CREATE TABLE x_$how (a int, b text);
			INSERT INTO x_$how SELECT g, repeat('x', 500) FROM generate_series(1, 200) g;
			CHECKPOINT;
		});
		my $x = sql(qq{SELECT pg_relation_filenode('x_$how')});
		my $xc;
		if ($how eq 'share')
		{
			$xc = sql(qq{SELECT test_bmap_share($x)});
		}
		else
		{
			my $root = sql(qq{SELECT test_bmap_root_capture($x)});
			$xc = sql(qq{SELECT test_bmap_share_root($root)});
			sql(qq{SELECT test_bmap_root_release($root)});
			sql(q{SELECT * FROM test_bmap_release_drain()});
		}
		sql(q{CHECKPOINT});
		my $before = sql(qq{SELECT test_bmap_read($x, 3, false)});
		my $reader = $node->background_psql('postgres');
		$reader->query_safe(q{SELECT injection_points_set_local()});
		$reader->query_safe(q{SELECT injection_points_attach('bmap-tree-find-root', 'wait')});
		$reader->query_until(qr/^$/, "SELECT test_bmap_read($xc, 3, true) AS seen \\gset\n");
		$node->wait_for_event('client backend', 'bmap-tree-find-root');
		# the copy's change copies the shared leaf; the source then has it alone
		sql(qq{SELECT test_bmap_write($xc, 5, 7)});
		sql(qq{SELECT test_bmap_write($x, 3, 99)});
		sql(q{CHECKPOINT});
		sql(q{SELECT injection_points_detach('bmap-tree-find-root')});
		sql(q{SELECT injection_points_wakeup('bmap-tree-find-root')});
		# (the old node's block may be made the source's page by then: read
		# as a node, it fails verification, warning so, and the lookup
		# begins again)
		is($reader->query(q{SELECT :'seen'}), $before,
			"a lookup holding a pointer from before its map copied a node reads its own block ($how)");
		is(sql(qq{SELECT test_bmap_read($x, 3, false)}), '99', "while the source has its change ($how)");
		$reader->quit;
	}
}

# Crash recovery keeps a relation's changes made before its node was
# stamped (the map's epoch raised, nothing shared: in place) or moved (a
# share since: to a new block), though the map's root page is written out
# after and the node's page not: a node's incarnation is kept by its birth,
# which recovery matches the root page's pointer against.
sql(q{CREATE EXTENSION IF NOT EXISTS pg_buffercache});
for my $how ('raised', 'shared')
{
	sql(qq{
		CREATE TABLE e_$how (k int, pad text);
		INSERT INTO e_$how SELECT g, repeat('p', 500) FROM generate_series(1, 100) g;
		CHECKPOINT;
	});
	my $e = sql(qq{SELECT pg_relation_filenode('e_$how')});
	sql(qq{UPDATE e_$how SET k = k + 1000 WHERE (ctid::text::point)[0] = 2});
	sql(qq{UPDATE e_$how SET k = k + 1000 WHERE (ctid::text::point)[0] = 2});
	if ($how eq 'raised')
	{
		sql(qq{SELECT test_bmap_raise_epoch($e)});
	}
	else
	{
		my $copy = sql(qq{SELECT test_bmap_share($e)});
		sql(qq{SELECT test_bmap_unlink($copy)});
		sql(q{SELECT * FROM test_bmap_release_drain()});
	}
	sql(qq{UPDATE e_$how SET k = k + 1000000 WHERE k = 99});
	my $sum = sql(qq{SELECT sum(k) FROM e_$how});
	sql(qq{SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
		WHERE relfilenode = $e AND relforknumber = 4 AND relblocknumber = 1});
	$node->stop('immediate');
	$node->start;
	is(sql(qq{SELECT sum(k) FROM e_$how}), $sum,
		"after a crash, the rows keep the changes made before their node was "
		  . ($how eq 'raised' ? 'stamped' : 'moved'));
}

$node->stop;
done_testing();
