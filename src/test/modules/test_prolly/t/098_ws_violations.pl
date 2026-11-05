# Copyright (c) 2026, PostgreSQL Global Development Group

# Rows of a merged root its constraints refuse, and what a merge cannot be.  Each scenario
# makes a base, and ours and theirs from it, merges them and checks the merge.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('violations');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";
$node->safe_psql('postgres', 'CREATE ROLE g_viol NOLOGIN TRAVELS');

my $T = q{CREATE TABLE t (id integer PRIMARY KEY, v integer)};
my $FK = q{CREATE TABLE p (id integer PRIMARY KEY); CREATE TABLE c (id integer PRIMARY KEY, pid integer REFERENCES p); INSERT INTO p VALUES (1), (2)};

# name, base, ours, theirs, the violations, a conflict's pattern if it is one
my @scenarios = (
	[ 'nothing refused', "$T; INSERT INTO t VALUES (1, 1)", q{INSERT INTO t VALUES (2, 2)}, q{INSERT INTO t VALUES (3, 3)}, [] ],
	[ 'a foreign key whose row went', $FK, q{DELETE FROM p WHERE id = 1}, q{INSERT INTO c VALUES (5, 1), (6, 2), (7, NULL)},
		[ 'foreign key c c_pid_fkey 5' ] ],
	[ 'a foreign key match full', q{CREATE TABLE p (a integer, b integer, PRIMARY KEY (a, b)); CREATE TABLE c (id integer PRIMARY KEY, x integer, y integer)},
		q{ALTER TABLE c ADD CONSTRAINT f FOREIGN KEY (x, y) REFERENCES p MATCH FULL}, q{INSERT INTO c VALUES (1, 1, NULL), (2, NULL, NULL)},
		[ 'foreign key c f 1' ] ],
	[ 'a foreign key match simple', q{CREATE TABLE p (a integer, b integer, PRIMARY KEY (a, b)); CREATE TABLE c (id integer PRIMARY KEY, x integer, y integer)},
		q{ALTER TABLE c ADD CONSTRAINT f FOREIGN KEY (x, y) REFERENCES p}, q{INSERT INTO c VALUES (1, 1, NULL), (2, 1, 1)},
		[ 'foreign key c f 2' ] ],
	[ 'a check one side added', "$T; INSERT INTO t VALUES (1, 1)", q{ALTER TABLE t ADD CONSTRAINT v_pos CHECK (v > 0)}, q{INSERT INTO t VALUES (2, -1), (3, NULL)},
		[ 'check t v_pos 2' ] ],
	[ 'a check not valid has none', "$T", q{ALTER TABLE t ADD CONSTRAINT v_pos CHECK (v > 0) NOT VALID}, q{INSERT INTO t VALUES (2, -1)}, [] ],
	[ 'a column made not null', "$T; INSERT INTO t VALUES (1, 1)", q{ALTER TABLE t ALTER COLUMN v SET NOT NULL}, q{INSERT INTO t VALUES (2, NULL)},
		[ 'not null t t_v_not_null 2' ] ],
	[ 'a value both sides took', "$T; ALTER TABLE t ADD CONSTRAINT v_one UNIQUE (v)", q{INSERT INTO t VALUES (1, 7)}, q{INSERT INTO t VALUES (2, 7)},
		[ 'unique t v_one 1', 'unique t v_one 2' ] ],
	[ 'nulls both sides took, distinct', "$T; ALTER TABLE t ADD CONSTRAINT v_one UNIQUE (v)", q{INSERT INTO t VALUES (1, NULL)}, q{INSERT INTO t VALUES (2, NULL)}, [] ],
	[ 'nulls both sides took, not distinct', "$T; ALTER TABLE t ADD CONSTRAINT v_one UNIQUE NULLS NOT DISTINCT (v)", q{INSERT INTO t VALUES (1, NULL)}, q{INSERT INTO t VALUES (2, NULL)},
		[ 'unique t v_one 1', 'unique t v_one 2' ] ],
	[ 'a unique index on an expression, partial', "$T; CREATE UNIQUE INDEX v_abs ON t (abs(v)) WHERE id < 100", q{INSERT INTO t VALUES (1, 7), (101, 7)}, q{INSERT INTO t VALUES (2, -7)},
		[ 'unique t v_abs 1', 'unique t v_abs 2' ] ],
	[ 'a key of two columns', q{CREATE TABLE t (a integer, b text, v integer UNIQUE, PRIMARY KEY (a, b))}, q{INSERT INTO t VALUES (1, 'x', 7)}, q{INSERT INTO t VALUES (1, 'y', 7)},
		[ 'unique t t_v_key 1,x', 'unique t t_v_key 1,y' ] ],
	[ 'keyless rows', q{CREATE TABLE t (a integer, b text UNIQUE)}, q{INSERT INTO t VALUES (1, 'x')}, q{INSERT INTO t VALUES (2, 'x')},
		[ 'unique t t_b_key (1,x)', 'unique t t_b_key (2,x)' ] ],
	[ 'a row too long to say', q{CREATE TABLE t (a text, b text UNIQUE)}, q{INSERT INTO t VALUES (repeat('x', 300), 'k')}, q{INSERT INTO t VALUES ('y', 'k')},
		[ 'unique t t_b_key ' . '(' . ('x' x 199) . ' ' . '@', 'unique t t_b_key (y,k)' ] ],
	[ 'an exclusion constraint', "$T; ALTER TABLE t ADD CONSTRAINT v_ex EXCLUDE USING btree (v WITH =)", q{INSERT INTO t VALUES (1, 7)}, q{INSERT INTO t VALUES (2, 7)},
		[ 'exclusion t v_ex' ] ],
	[ 'a check of a partitioned table', q{CREATE TABLE pt (id integer PRIMARY KEY, v integer) PARTITION BY RANGE (id); CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100); CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (100) TO (200)},
		q{ALTER TABLE pt ADD CONSTRAINT v_pos CHECK (v > 0)}, q{INSERT INTO pt VALUES (1, -1), (101, 1)}, [ 'check pt1 v_pos 1' ] ],
	[ 'a partition not null', q{CREATE TABLE pt (id integer PRIMARY KEY, v integer) PARTITION BY RANGE (id); CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100)},
		q{ALTER TABLE pt ALTER COLUMN v SET NOT NULL}, q{INSERT INTO pt VALUES (1, NULL)}, [ 'not null pt1 pt_v_not_null 1' ] ],
	[ 'a foreign key to partitions', q{CREATE TABLE p (id integer PRIMARY KEY) PARTITION BY RANGE (id); CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (100); INSERT INTO p VALUES (1), (2); CREATE TABLE c (id integer PRIMARY KEY, pid integer REFERENCES p)},
		q{DELETE FROM p WHERE id = 1}, q{INSERT INTO c VALUES (5, 1), (6, 2)}, [ 'foreign key c c_pid_fkey 5' ] ],
	[ 'a key whose values print apart', q{CREATE TABLE t (id numeric PRIMARY KEY)}, q{INSERT INTO t VALUES (1.0)}, q{INSERT INTO t VALUES (1.00)}, undef, qr/^unapplied .*duplicate|^unapplied .*unique/ ],
	[ 'a view of what the other dropped', "$T", q{ALTER TABLE t DROP COLUMN v}, q{CREATE VIEW w AS SELECT v FROM t}, undef, qr/^unapplied object "w" does not make that object/ ],
	[ 'a check that raises for a row', "$T", q{ALTER TABLE t ADD CONSTRAINT z CHECK (1 / v > 0)}, q{INSERT INTO t VALUES (1, 0)}, undef, qr/^unapplied division by zero/ ],
);

my $i = 0;
for my $case (@scenarios)
{
	$i++;
	my ($name, $base, $ours, $theirs, $violations, $conflict) = @$case;
	my ($rc, $out, $err) = $node->psql('postgres', qq{
	SET client_min_messages = warning;
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_viol;
	CREATE SCHEMA o$i VERSIONED; ALTER SCHEMA o$i OWNER TO g_viol;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_viol;
	SET ROLE g_viol;
	SET search_path = b$i; $base;
	SET search_path = o$i; $base; $ours;
	SET search_path = t$i; $base; $theirs;
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('b$i') AS rb \\gset
	SELECT test_ws_root_capture('o$i') AS ro \\gset
	SELECT test_ws_root_capture('t$i') AS rt \\gset
	SELECT test_ws_merge_checked(:'rb', :'ro', :'rt', ARRAY['o$i', 't$i', 'b$i']) AS merged \\gset
	\\echo =====merge
	\\echo :merged
	\\echo =====end
	SELECT split_part(:'merged', E'\\n', 1) AS m \\gset
	\\echo =====viol
	SELECT test_ws_violations(:'m');
	\\echo =====end
	\\echo =====added
	SELECT test_ws_diff(:'ro', :'m');
	\\echo =====end
	\\echo =====resolved
	SELECT test_ws_diff(:'m', :'ro');
	\\echo =====end
	});
	is($rc, 0, "$name: runs") or diag($err);
	my ($merge) = $out =~ /=====merge\n(.*?)\n?=====end/s;
	my @conflicts = split /\n/, $merge // '';
	shift @conflicts;
	if (defined $conflict)
	{
		is(scalar @conflicts, 1, "$name: a conflict") or diag($merge);
		like($conflicts[0] // '', $conflict, "$name: what it says");
		next;
	}
	is_deeply(\@conflicts, [], "$name: no conflict");
	my ($viol) = $out =~ /=====viol\n(.*?)\n?=====end/s;
	my @got = map { s/^(.*\(x+) [0-9a-f]{64}$/$1 @/r } split /\n/, $viol // '';
	is_deeply(\@got, $violations, "$name: its violations");

	# as diff says, one way added and the other resolved
	my ($added) = $out =~ /=====added\n(.*?)\n?=====end/s;
	my ($resolved) = $out =~ /=====resolved\n(.*?)\n?=====end/s;
	is(scalar(() = ($added // '') =~ /^violation added /mg), scalar @$violations, "$name: added in diff");
	is(scalar(() = ($resolved // '') =~ /^violation resolved /mg), scalar @$violations, "$name: resolved in diff");
}

# one line of each, as diff prints it
my $lines = $node->safe_psql('postgres', q{
	SET client_min_messages = warning;
	CREATE SCHEMA lb VERSIONED; CREATE SCHEMA lo VERSIONED; CREATE SCHEMA lt VERSIONED;
	SET search_path = lb; CREATE TABLE t (id integer PRIMARY KEY, v integer, CONSTRAINT v_ex EXCLUDE USING btree (v WITH =));
	SET search_path = lo; CREATE TABLE t (id integer PRIMARY KEY, v integer, CONSTRAINT v_ex EXCLUDE USING btree (v WITH =)); ALTER TABLE t ADD CONSTRAINT v_pos CHECK (v > 0); INSERT INTO t VALUES (1, 7);
	SET search_path = lt; CREATE TABLE t (id integer PRIMARY KEY, v integer, CONSTRAINT v_ex EXCLUDE USING btree (v WITH =)); INSERT INTO t VALUES (2, -7), (3, 7);
	SET search_path = public;
	SELECT test_ws_diff(test_ws_root_capture('lo'), split_part(test_ws_merge_checked(test_ws_root_capture('lb'), test_ws_root_capture('lo'), test_ws_root_capture('lt'), ARRAY['lo', 'lt', 'lb']), E'\n', 1));
});
like($lines, qr/^violation added v_pos t 2$/m, 'a row, as diff says it');
like($lines, qr/^violation added v_ex t$/m, 'a constraint alone, as diff says it');

$node->stop;
done_testing();
