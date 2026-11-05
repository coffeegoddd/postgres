# Copyright (c) 2026, PostgreSQL Global Development Group

# A root holding violations, restored: what is set aside stays out of the table until it
# holds, comes back whole once it does, and only that clears it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('violrestore');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";
$node->safe_psql('postgres', 'CREATE ROLE g_vr NOLOGIN TRAVELS');

# A merge of base, then ours and theirs, restored into schema d$n; each's text runs there.
my $n = 0;

sub merged
{
	my ($base, $ours, $theirs, @then) = @_;
	$n++;
	my ($rc, $out, $err) = $node->psql('postgres', qq{
	SET client_min_messages = warning;
	CREATE SCHEMA b$n VERSIONED; ALTER SCHEMA b$n OWNER TO g_vr;
	CREATE SCHEMA o$n VERSIONED; ALTER SCHEMA o$n OWNER TO g_vr;
	CREATE SCHEMA t$n VERSIONED; ALTER SCHEMA t$n OWNER TO g_vr;
	CREATE SCHEMA d$n VERSIONED; ALTER SCHEMA d$n OWNER TO g_vr;
	SET ROLE g_vr;
	SET search_path = b$n; $base;
	SET search_path = o$n; $base; $ours;
	SET search_path = t$n; $base; $theirs;
	RESET ROLE; SET search_path = public;
	SELECT split_part(test_ws_merge_checked(test_ws_root_capture('b$n'), test_ws_root_capture('o$n'),
		test_ws_root_capture('t$n')), E'\\n', 1) AS m \\gset
	SELECT test_ws_root_restore(:'m', 'd$n') IS NULL AS r \\gset
	DROP SCHEMA b$n, o$n, t$n CASCADE;
	SET search_path = d$n, public;
	\\echo =====same
	SELECT test_ws_root_capture('d$n') = :'m';
	\\echo =====end
	} . join('', map { "\\echo =====step\n$_;\n\\echo =====end\n" } @then));
	return ($rc, $out, $err);
}

sub steps
{
	my ($out) = @_;
	my @got;
	while ($out =~ /=====step\n(.*?)\n?=====end/sg) { push @got, $1; }
	return @got;
}

# what goes with a unique constraint, and a key that needs it
my ($rc, $out, $err) = merged(
	q{CREATE TABLE p (id integer PRIMARY KEY, code integer NOT NULL, CONSTRAINT code_one UNIQUE (code) WITH (fillfactor = 70));
	  COMMENT ON CONSTRAINT code_one ON p IS 'one each';
	  ALTER TABLE p REPLICA IDENTITY USING INDEX code_one;
	  CREATE TABLE c (id integer PRIMARY KEY, code integer REFERENCES p (code))},
	q{INSERT INTO p VALUES (1, 7)}, q{INSERT INTO p VALUES (2, 7)},
	q{SELECT count(*) FROM pg_constraint WHERE conrelid IN ('p'::regclass, 'c'::regclass) AND contype IN ('u', 'f')},
	q{SELECT defkind, defname FROM pg_ws_deferred ORDER BY 1, 2},
	q{INSERT INTO p VALUES (3, 7)},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT test_ws_violations(test_ws_root_capture(current_schema))},
	q{DELETE FROM p WHERE id > 1},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT count(*) FROM pg_ws_deferred},
	q{SELECT conname, contype FROM pg_constraint WHERE conrelid IN ('p'::regclass, 'c'::regclass) AND contype IN ('u', 'f') ORDER BY 1},
	q{SELECT obj_description(oid, 'pg_constraint') FROM pg_constraint WHERE conname = 'code_one'},
	q{SELECT relreplident FROM pg_class WHERE oid = 'p'::regclass},
	q{SELECT reloptions FROM pg_class WHERE oid = 'code_one'::regclass},
	q{SELECT test_ws_violations(test_ws_root_capture(current_schema))});
is($rc, 0, 'a unique constraint set aside: runs') or diag($err);
like($out, qr/=====same\nt\n/, 'restored, the root the merge made');
my @s = steps($out);
is($s[0], '0', 'the unique constraint and the key that needs it are not on the tables');
is($s[1], "f|c_code_fkey\nu|code_one", 'they are set aside');
is($s[3], '2', 'with rows still alike, resolving leaves them');
like($s[4], qr/^unique p code_one 1\nunique p code_one 2\nunique p code_one 3$/, 'a row written since is one of them');
is($s[6], '0', 'with the rows fixed, nothing is left');
is($s[7], '0', 'nor set aside');
is($s[8], "c_code_fkey|f\ncode_one|u", 'both are on their tables');
is($s[9], 'one each', 'its comment is back');
is($s[10], 'i', 'and the replica identity through it');
is($s[11], '{fillfactor=70}', 'and its settings');
is($s[12], '', 'nothing is violated');

# a violation of each other kind, and a column renamed while it waits
($rc, $out, $err) = merged(
	q{CREATE TABLE t (id integer PRIMARY KEY, v integer)},
	q{ALTER TABLE t ADD CONSTRAINT v_pos CHECK (v > 0); ALTER TABLE t ALTER COLUMN v SET NOT NULL; INSERT INTO t VALUES (1, 1)},
	q{INSERT INTO t VALUES (2, -1), (3, NULL)},
	q{SELECT count(*) FROM pg_constraint WHERE conrelid = 't'::regclass AND contype IN ('c', 'n') AND conname <> 't_id_not_null'},
	q{SELECT test_ws_violations(test_ws_root_capture(current_schema))},
	q{ALTER TABLE t RENAME COLUMN v TO w},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{UPDATE t SET w = 5},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'v_pos'},
	q{SELECT attnotnull FROM pg_attribute WHERE attrelid = 't'::regclass AND attname = 'w'});
is($rc, 0, 'a check and a not null set aside: runs') or diag($err);
@s = steps($out);
is($s[0], '0', 'neither is on the table, which a plan would trust');
is($s[1], "check t v_pos 2\nnot null t t_v_not_null 3", 'what each refuses');
is($s[3], '2', 'with no row fixed, both wait');
is($s[5], '0', 'with them fixed, neither does');
is($s[6], 'CHECK ((w > 0))', 'the check, by the column as renamed');
is($s[7], 't', 'and not null');

# an exclusion constraint and a unique index
($rc, $out, $err) = merged(
	q{CREATE TABLE t (id integer PRIMARY KEY, v integer, w integer, CONSTRAINT v_ex EXCLUDE USING btree (v WITH =)); CREATE UNIQUE INDEX w_one ON t (w) WHERE w > 0},
	q{INSERT INTO t VALUES (1, 7, 7)}, q{INSERT INTO t VALUES (2, 7, 7)},
	q{SELECT defkind, defname FROM pg_ws_deferred ORDER BY 1, 2},
	q{UPDATE t SET v = 8, w = 8 WHERE id = 2},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT indexdef FROM pg_indexes WHERE indexname = 'w_one'});
is($rc, 0, 'an exclusion and an index set aside: runs') or diag($err);
@s = steps($out);
is($s[0], "i|w_one\nx|v_ex", 'both set aside');
is($s[2], '0', 'and resolved');
like($s[3], qr/^CREATE UNIQUE INDEX w_one ON \S+ USING btree \(w\) WHERE \(w > 0\)$/, 'the index, as it was');

# a table dropped takes what is set aside of it
($rc, $out, $err) = merged(
	q{CREATE TABLE t (id integer PRIMARY KEY, v integer UNIQUE)},
	q{INSERT INTO t VALUES (1, 7)}, q{INSERT INTO t VALUES (2, 7)},
	q{DROP TABLE t},
	q{SELECT count(*) FROM pg_ws_deferred});
is($rc, 0, 'a table dropped: runs') or diag($err);
@s = steps($out);
is($s[1], '0', 'nothing is left set aside');

# a partition's cannot yet be set aside: refused, not wrong
($rc, $out, $err) = merged(
	q{CREATE TABLE pt (id integer PRIMARY KEY, v integer) PARTITION BY RANGE (id); CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100)},
	q{ALTER TABLE pt ADD CONSTRAINT v_pos CHECK (v > 0)}, q{INSERT INTO pt VALUES (1, -1)});
isnt($rc, 0, 'a partition with a violation is not restored');
like($err, qr/is partitioned or inherits/, 'and says why');

$node->stop;
done_testing();
