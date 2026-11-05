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
is($s[1], "f|c_code_fkey\nr|code_one\nu|code_one", 'they are set aside, with the replica identity through it');
is($s[3], '3', 'with rows still alike, resolving leaves them');
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

# a resolved schema captures as a merge that never had the violation, for what prints otherwise
my $CHECK = q{CREATE TABLE t (id integer PRIMARY KEY, i integer, v varchar(10), w integer)};
my $held = q{ALTER TABLE t ADD CONSTRAINT c1 CHECK (i > 0 OR ((v IN ('a', 'b')) IS NULL)); ALTER TABLE t ADD CONSTRAINT w_one UNIQUE (w)};
$n++;
my ($c_rc, $c_out, $c_err) = $node->psql('postgres', qq{
	SET client_min_messages = warning;
	CREATE SCHEMA b$n VERSIONED; ALTER SCHEMA b$n OWNER TO g_vr; CREATE SCHEMA o$n VERSIONED; ALTER SCHEMA o$n OWNER TO g_vr;
	CREATE SCHEMA t$n VERSIONED; ALTER SCHEMA t$n OWNER TO g_vr; CREATE SCHEMA d$n VERSIONED; ALTER SCHEMA d$n OWNER TO g_vr;
	CREATE SCHEMA e$n VERSIONED; ALTER SCHEMA e$n OWNER TO g_vr; CREATE SCHEMA f$n VERSIONED; ALTER SCHEMA f$n OWNER TO g_vr;
	SET ROLE g_vr;
	SET search_path = b$n; $CHECK; INSERT INTO t VALUES (1, 1, NULL, 1);
	SET search_path = o$n; $CHECK; INSERT INTO t VALUES (1, 1, NULL, 1); $held;
	SET search_path = t$n; $CHECK; INSERT INTO t VALUES (1, 1, NULL, 1); INSERT INTO t VALUES (2, -1, 'z', 1);
	SET search_path = e$n; $CHECK; INSERT INTO t VALUES (1, 1, NULL, 1); INSERT INTO t VALUES (2, 1, 'z', 2);
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('b$n') AS rb \\gset
	SELECT test_ws_root_capture('o$n') AS ro \\gset
	SELECT test_ws_root_capture('t$n') AS rt \\gset
	SELECT test_ws_root_capture('e$n') AS re \\gset
	SELECT split_part(test_ws_merge_checked(:'rb', :'ro', :'rt'), E'\\n', 1) AS m \\gset
	SELECT split_part(test_ws_merge_checked(:'rb', :'ro', :'re'), E'\\n', 1) AS clean \\gset
	SELECT test_ws_root_restore(:'m', 'd$n') IS NULL AS r \\gset
	SELECT test_ws_root_restore(:'clean', 'f$n') IS NULL AS r \\gset
	UPDATE d$n.t SET i = 1, w = 2 WHERE id = 2;
	\\echo =====step
	SELECT test_ws_viol_resolve('d$n');
	\\echo =====end
	\\echo =====step
	SELECT test_ws_root_capture('d$n') = :'clean';
	\\echo =====end
	\\echo =====step
	SELECT test_ws_root_capture('f$n') = :'clean';
	\\echo =====end
});
is($c_rc, 0, 'a resolved schema against a clean one: runs') or diag($c_err);
@s = steps($c_out);
is($s[0], '0', 'resolved');
is($s[2], 't', 'the clean one captures as itself');
is($s[1], 't', 'and the resolved one as the root a merge with no violation makes');

# each scenario's restored root captures as itself, then resolves
my @whole = (
	[ 'a comment on a constraint and on its index',
		q{CREATE TABLE t (id integer PRIMARY KEY, v integer, CONSTRAINT zz UNIQUE (v)); COMMENT ON CONSTRAINT zz ON t IS 'c'; COMMENT ON INDEX zz IS 'i'},
		q{SELECT obj_description('zz'::regclass, 'pg_class') || obj_description((SELECT oid FROM pg_constraint WHERE conname = 'zz'), 'pg_constraint')}, 'ic' ],
	[ 'clustering and a replica identity on two set aside',
		q{CREATE TABLE t (id integer PRIMARY KEY, v integer NOT NULL, CONSTRAINT a UNIQUE (v)); CREATE UNIQUE INDEX b ON t (v, id); ALTER TABLE t CLUSTER ON a; ALTER TABLE t REPLICA IDENTITY USING INDEX b},
		q{SELECT (SELECT string_agg(c.relname, ',' ORDER BY c.relname) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 't'::regclass AND (i.indisclustered OR i.indisreplident))}, 'a,b' ],
);
for my $case (@whole)
{
	my ($name, $base, $query, $want) = @$case;
	($rc, $out, $err) = merged($base, q{INSERT INTO t VALUES (1, 7)}, q{INSERT INTO t VALUES (2, 7)},
		q{UPDATE t SET v = 8 WHERE id = 2},
		q{SELECT test_ws_viol_resolve(current_schema)}, $query,
		q{SELECT test_ws_violations(test_ws_root_capture(current_schema))});
	is($rc, 0, "$name: runs") or diag($err);
	like($out, qr/=====same\nt\n/, "$name: restored, the root the merge made");
	@s = steps($out);
	is($s[1], '0', "$name: resolved");
	is($s[2], $want, "$name: what goes with them is back");
	is($s[3], '', "$name: nothing is violated");
}

($rc, $out, $err) = merged(q{CREATE TABLE t (id integer PRIMARY KEY, v integer, w integer); CREATE UNIQUE INDEX u ON t (v); INSERT INTO t VALUES (1, 1, 1)},
	q{ALTER TABLE t ALTER COLUMN v SET NOT NULL; ALTER TABLE t REPLICA IDENTITY USING INDEX u},
	q{INSERT INTO t VALUES (2, NULL, 2)},
	q{SELECT relreplident FROM pg_class WHERE oid = 't'::regclass},
	q{UPDATE t SET v = 2 WHERE id = 2},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT relreplident FROM pg_class WHERE oid = 't'::regclass});
is($rc, 0, 'a replica identity needing a not null set aside: runs') or diag($err);
like($out, qr/=====same\nt\n/, 'restored, the root the merge made');
@s = steps($out);
is($s[0], 'd', 'it waits');
is($s[2], '0', 'and resolves');
is($s[3], 'i', 'and is back');

($rc, $out, $err) = merged(q{CREATE TABLE t (id integer PRIMARY KEY, v integer); CREATE UNIQUE INDEX foo ON t (id, v) WITH (fillfactor = 70); COMMENT ON INDEX foo IS 'i'},
	q{ALTER TABLE t ADD CONSTRAINT foo CHECK (v > 0); COMMENT ON CONSTRAINT foo ON t IS 'k'}, q{INSERT INTO t VALUES (1, -1)},
	q{SELECT test_ws_violations(test_ws_root_capture(current_schema))},
	q{UPDATE t SET v = 1},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT obj_description((SELECT oid FROM pg_constraint WHERE conname = 'foo'), 'pg_constraint') || obj_description('foo'::regclass, 'pg_class')});
is($rc, 0, 'a check and an index of one name: runs') or diag($err);
like($out, qr/=====same\nt\n/, 'restored, the root the merge made');
@s = steps($out);
is($s[0], 'check t foo 1', 'only the check is violated');
is($s[2], '0', 'and resolves');
is($s[3], 'ki', 'each its own comment');

# tables renamed while a foreign key waits
for my $renamed ([ 'its table', 'ALTER TABLE c RENAME TO c2', 'c2' ], [ 'what it references', 'ALTER TABLE p RENAME TO p2', 'c' ])
{
	my ($what, $rename, $child) = @$renamed;
	($rc, $out, $err) = merged(q{CREATE TABLE p (id integer PRIMARY KEY); CREATE TABLE c (id integer PRIMARY KEY, pid integer REFERENCES p); INSERT INTO p VALUES (1), (2)},
		q{DELETE FROM p WHERE id = 1}, q{INSERT INTO c VALUES (5, 1)},
		$rename,
		q{SELECT test_ws_violations(test_ws_root_capture(current_schema))},
		qq{UPDATE $child SET pid = 2},
		q{SELECT test_ws_viol_resolve(current_schema)},
		q{SELECT count(*) FROM pg_constraint WHERE contype = 'f' AND connamespace = current_schema::regnamespace});
	is($rc, 0, "$what renamed: runs") or diag($err);
	@s = steps($out);
	like($s[1], qr/^foreign key \w+ c_pid_fkey 5$/, "$what renamed: still says it");
	is($s[3], '0', "$what renamed: resolves");
	is($s[4], '1', "$what renamed: and is back");
}

# what cannot be done while it waits says so
($rc, $out, $err) = merged(q{CREATE TABLE t (id integer PRIMARY KEY, v integer, CONSTRAINT vu UNIQUE (v))},
	q{INSERT INTO t VALUES (1, 7)}, q{INSERT INTO t VALUES (2, 7)},
	q{DELETE FROM t WHERE id = 2}, q{ALTER TABLE t ADD CONSTRAINT vu UNIQUE (v)},
	q{SELECT test_ws_root_capture(current_schema)});
like($err, qr/has "vu" of its own and set aside/, 'another of a set-aside name: refused by name');

($rc, $out, $err) = merged(q{CREATE TABLE p (id integer PRIMARY KEY); CREATE TABLE c (id integer PRIMARY KEY, pid integer REFERENCES p); INSERT INTO p VALUES (1)},
	q{DELETE FROM p}, q{INSERT INTO c VALUES (5, 1)},
	q{DROP TABLE p}, q{SELECT test_ws_root_capture(current_schema)});
like($err, qr/is set aside, and the table it references is gone/, 'what a key references, dropped: says so');

# what raises for a row waits, and what else holds does not wait for it
($rc, $out, $err) = merged(q{CREATE TABLE a (id integer PRIMARY KEY, v integer, CONSTRAINT vu UNIQUE (v)); CREATE TABLE z (id integer PRIMARY KEY, w integer)},
	q{INSERT INTO a VALUES (1, 7); ALTER TABLE z ADD CONSTRAINT zc CHECK (1 / w > 0)}, q{INSERT INTO a VALUES (2, 7); INSERT INTO z VALUES (1, -1)},
	q{SELECT defkind, defname FROM pg_ws_deferred ORDER BY 1, 2},
	q{DELETE FROM a WHERE id = 2; UPDATE z SET w = 0},
	q{SELECT test_ws_viol_resolve(current_schema)},
	q{SELECT defname FROM pg_ws_deferred WHERE defrelid IN ('a'::regclass, 'z'::regclass) ORDER BY 1});
is($rc, 0, 'what raises: runs') or diag($err);
@s = steps($out);
is($s[2], '1', 'one waits');
is($s[3], 'zc', 'the one that raises, and only it');
like($err, qr/"zc" of table "z" stays set aside/, 'with a warning of why');

# capturing what is set aside asks nothing of roles
($rc, $out, $err) = merged(q{CREATE TABLE t (id integer PRIMARY KEY, v integer, CONSTRAINT vu UNIQUE (v))},
	q{INSERT INTO t VALUES (1, 7)}, q{INSERT INTO t VALUES (2, 7)},
	q{SELECT test_ws_violations(test_ws_root_capture(current_schema))});
is($rc, 0, 'capturing what is set aside asks nothing of roles') or diag($err);

$node->stop;
done_testing();
