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
# than copied (it used to fail "is copied but held once").
SKIP:
{
	skip 'injection points not supported by this build', 7
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
	$first->query_until(qr/^$/, "UPDATE u SET n = 1 WHERE k = 10000;\n");
	$node->wait_for_event('client backend', 'bmap-tree-copy-counts');
	sql(q{UPDATE v SET n = 2 WHERE k = 10000});
	sql(q{SELECT injection_points_wakeup('bmap-tree-copy-counts')});
	my $done = eval {
		$first->query_safe(q{SELECT injection_points_detach('bmap-tree-copy-counts')});
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
}

$node->stop;
done_testing();
