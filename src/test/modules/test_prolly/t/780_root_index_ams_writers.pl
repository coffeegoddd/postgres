# Copyright (c) 2026, PostgreSQL Global Development Group

# Every index access method under roots, with writers running while the
# commits capture them: GiST, GIN (a pending list too), BRIN, SP-GiST,
# hash, bloom (generic WAL) and btree.  Each commit's root, once reset to,
# has the commit's rows (as its tree has them) and indexes that find just
# those, amcheck's checks passing; after a crash, and on a standby, every
# page redo wrote checked against the primary's (wal_consistency_checking).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use IPC::Run;
use Time::HiRes qw(usleep);
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root_index_ams_writers');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_consistency_checking = 'all'
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('root_index_ams_writers_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "wal_consistency_checking = 'all'");
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION amcheck; CREATE EXTENSION bloom;
	CREATE SCHEMA ri VERSIONED;
	CREATE TABLE ri.t (id int PRIMARY KEY, v int NOT NULL, p point NOT NULL,
		q point NOT NULL, r int4range NOT NULL, tags int[] NOT NULL,
		doc tsvector NOT NULL, txt text NOT NULL, s int NOT NULL,
		b1 int NOT NULL, b2 int NOT NULL, bt int NOT NULL)
		WITH (fillfactor = 70, autovacuum_enabled = off);
	CREATE INDEX t_gist_p ON ri.t USING gist (p);
	CREATE INDEX t_gist_r ON ri.t USING gist (r);
	CREATE INDEX t_gin_tags ON ri.t USING gin (tags) WITH (fastupdate = off);
	CREATE INDEX t_gin_doc ON ri.t USING gin (doc) WITH (fastupdate = on);
	CREATE INDEX t_brin_s ON ri.t USING brin (s) WITH (pages_per_range = 2);
	CREATE INDEX t_spgist_q ON ri.t USING spgist (q);
	CREATE INDEX t_spgist_txt ON ri.t USING spgist (txt);
	CREATE INDEX t_hash_v ON ri.t USING hash (v);
	CREATE INDEX t_bloom ON ri.t USING bloom (b1, b2);
	CREATE INDEX t_btree ON ri.t (bt);
	CREATE FUNCTION ri.ri_row(g int, tag text) RETURNS ri.t LANGUAGE sql AS $$
		SELECT ROW(g, g % 50, point(g % 97, g / 97), point(g / 97, g % 89),
			int4range(g, g + g % 20 + 1), ARRAY[g % 31, g % 37 + 100],
			to_tsvector('simple', 'w' || (g % 41) || ' x' || (g % 43) || ' ' || tag),
			'k' || (g % 61) || tag, g, g % 53, g % 59, g % 71)::ri.t
	$$;
	INSERT INTO ri.t SELECT (ri.ri_row(g, 'base')).* FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'base')});

# probes: each index's query, answered by its index (bitmap scans, which
# every access method has), and by a sequential scan
my @probes = (
	[ 't_gist_p', q{p <@ box '((10,5),(40,30))'} ],
	[ 't_gist_r', q{r && int4range(1500, 1530)} ],
	[ 't_gin_tags', q{tags @> ARRAY[7]} ],
	[ 't_gin_doc', q{doc @@ to_tsquery('simple', 'w3 & x5')} ],
	[ 't_brin_s', q{s BETWEEN 1200 AND 1900} ],
	[ 't_spgist_q', q{q <@ box '((10,20),(40,60))'} ],
	[ 't_spgist_txt', q{txt ^@ 'k17'} ],
	[ 't_hash_v', q{v = 13} ],
	[ 't_bloom', q{b1 = 21 AND b2 = 30} ],
	[ 't_btree', q{bt BETWEEN 3 AND 5} ]);

# the probes whose index finds other rows than a sequential scan, or
# whose plan doesn't scan the index
sub disagree
{
	my ($n) = @_;
	my @bad;
	foreach my $p (@probes)
	{
		my ($idx, $w) = @$p;
		my $q = "SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '') FROM ri.t WHERE $w";
		my $byidx = "SET enable_seqscan = off; SET enable_indexscan = off;";
		my $seq = $n->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; $q");
		my $plan = $n->safe_psql('postgres', "$byidx EXPLAIN (COSTS OFF) $q");
		if ($plan !~ /Bitmap Index Scan on $idx\b/)
		{
			push @bad, "$idx not used: $plan";
			next;
		}
		push @bad, $idx if $seq ne $n->safe_psql('postgres', "$byidx $q");
	}
	return join('; ', @bad);
}

# amcheck's checks of the btree (every row found) and GIN indexes
sub checked
{
	my ($n, $what) = @_;
	my ($ret, $out, $err) = $n->psql('postgres', q{
		SELECT bt_index_check(indexrelid, true) FROM pg_index i
			JOIN pg_class c ON c.oid = i.indexrelid
		WHERE indrelid = 'ri.t'::regclass AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'btree');
		SELECT gin_index_check(indexrelid) FROM pg_index i
			JOIN pg_class c ON c.oid = i.indexrelid
		WHERE indrelid = 'ri.t'::regclass AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'gin')},
		on_error_stop => 1);
	is($ret, 0, "$what: amcheck finds nothing wrong");
	diag($err) if $ret != 0;
}

my $digest = q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM ri.t t};
sub digest_at
{
	my ($c) = @_;
	return sql("SELECT md5(string_agg(t::text, ',' ORDER BY id))
		FROM dolt_at('$c', NULL::ri.t) t");
}

# writers: inserts, updates of every indexed column, deletes, a rollback
# now and then; each transaction short, with a pause after it, so that the
# commits' keeps find the table between them
my $script = $node->basedir . '/writers.sql';
open(my $fh, '>', $script) or die;
print $fh q{\set g random(1, 6000)
\set k random(1, 10)
BEGIN;
INSERT INTO ri.t SELECT (ri.ri_row(:g + 10000 * :client_id + 100000, 'w')).* ON CONFLICT DO NOTHING;
UPDATE ri.t SET v = v + 1, q = point(q[0], q[1] + 0.25), tags = tags || :k,
	txt = txt || 'u', s = s + 1, b1 = b1 + 1, bt = bt + 1,
	doc = doc || to_tsvector('simple', 'u' || :k) WHERE id = :g;
DELETE FROM ri.t WHERE id = :g + 7 AND :k < 3;
END;
SELECT pg_sleep(0.05);
};
close($fh);

my $captured_q = q{SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'bmap_roots_captured'), 0)};
my $captured_before = sql($captured_q);
my ($out, $err) = ('', '');
my $writers = IPC::Run::start(
	[ 'pgbench', '-n', '-h', $node->host, '-p', $node->port,
	  '-c', '3', '-T', '8', '-f', $script, 'postgres' ],
	'>' => \$out, '2>' => \$err);

# commits, as the writers write: each one's hash
my @commits;
for my $i (1 .. 8)
{
	usleep(400_000);
	my $c = sql(qq{SELECT (dolt_commit('--allow-empty', '-am', 'c$i'))[1]});
	push @commits, $c if $c ne '';
	usleep(400_000);
}
$writers->finish;
like($out, qr/number of failed transactions: 0/, 'the writers ran with no failure')
	or diag($err);
cmp_ok(sql($captured_q), '>', $captured_before, 'roots captured meanwhile');
cmp_ok(scalar(@commits), '>=', 4, 'commits made meanwhile');
my $last = sql(q{SELECT (dolt_commit('--allow-empty', '-am', 'last'))[1]});
push @commits, $last if $last ne '';
is(disagree($node), '', 'every index agrees after the writers');

# each commit, newest first, reset to: its rows, every index agreeing
my $restored_q = q{SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'), 0)};
my $restored = sql($restored_q);
my $n = scalar(@commits);
for my $k (1 .. 3)
{
	my $c = $commits[ $n - 1 - $k ];
	sql(qq{SELECT dolt_reset('--hard', '$c')});
	is(sql($digest), digest_at($c), "reset to commit -$k: its rows");
	is(disagree($node), '', "reset to commit -$k: every index agrees");
	checked($node, "reset to commit -$k");
}
cmp_ok(sql($restored_q), '>', $restored, 'the resets restored relations of roots');
my $at = sql($digest);

# writes after, VACUUM, a commit; then a crash: redo replays the roots'
# captures and the copies made of their pages, every page checked
sql(q{INSERT INTO ri.t SELECT (ri.ri_row(g, 'after')).* FROM generate_series(200001, 200400) g;
	UPDATE ri.t SET bt = bt + 3, q = point(1, 1) WHERE id % 9 = 0;
	DELETE FROM ri.t WHERE id % 23 = 0});
sql(q{VACUUM ri.t});
my $after = sql(q{SELECT (dolt_commit('-am', 'after'))[1]});
$node->stop('immediate');
$node->start;
is(disagree($node), '', 'after a crash, every index agrees');
checked($node, 'after a crash');
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql($digest), $at, 'after a crash, a reset gives the commit before\'s rows');
is(disagree($node), '', 'and every index agrees');
checked($node, 'after a crash, reset');

# the standby replayed it all, page by page checked, and reads the same
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', $digest), $at, 'the standby reads the same rows');
is(disagree($standby), '', 'on the standby, every index agrees');

$standby->stop;
$node->stop;
done_testing();
