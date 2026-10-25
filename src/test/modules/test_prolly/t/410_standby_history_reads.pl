# Copyright (c) 2026, PostgreSQL Global Development Group

# History reads on a hot standby are read-only: dolt_at, dolt_diff (by row
# type and by name), dolt_diff_table, dolt_history_table, dolt_log,
# dolt_status and a history mount made on the primary each answer there as
# the primary does, assigning no transaction id.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('standby_history');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA sh VERSIONED;
	CREATE TABLE sh.t (id int PRIMARY KEY, v text);
	INSERT INTO sh.t SELECT g, 'v' || g FROM generate_series(1, 50) g;
	SELECT dolt_commit('-Am', 'loaded');
	UPDATE sh.t SET v = 'changed' WHERE id % 10 = 0;
	DELETE FROM sh.t WHERE id = 5;
	INSERT INTO sh.t VALUES (51, 'new');
	SELECT dolt_commit('-am', 'changed');
	UPDATE sh.t SET v = 'working' WHERE id = 1});
sql(q{SELECT mount FROM prolly_mount('HEAD', 'sh', 'hm', NULL, false, true)});

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby_history_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

# more history made once the standby streams: a commit, a keyless table
# and long values, read from the standby's replay alone
sql(q{CREATE TABLE sh.k (v text, j jsonb);
	INSERT INTO sh.k SELECT repeat('x' || g, 3000), jsonb_build_object('a', repeat('y', 5000), 'g', g)
		FROM generate_series(1, 5) g;
	SELECT dolt_commit('-Am', 'keyless');
	UPDATE sh.k SET j = jsonb_set(j, '{g}', '0') WHERE (j->>'g')::int = 2;
	SELECT dolt_commit('-am', 'keyless changed');
	SELECT dolt_tag('v1', 'HEAD~1');
	SELECT dolt_branch('side', 'HEAD~2');
	UPDATE sh.t SET v = 'working' WHERE id = 2});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

my %reads = (
	'dolt_at of long values' => q{SELECT string_agg(md5(v) || md5(j::text), ',' ORDER BY v)
		FROM dolt_at('HEAD~1', NULL::sh.k)},
	'dolt_diff of long values' => q{SELECT string_agg(diff_type || md5((to_row).j::text), ',' ORDER BY 1)
		FROM dolt_diff('HEAD~1', 'HEAD', NULL::sh.k)},
	'dolt_diff_stat' => q{SELECT string_agg(table_name || rows_added || rows_deleted || rows_modified, ',' ORDER BY 1)
		FROM dolt_diff_stat('HEAD~3', 'HEAD')},
	'dolt_schema_diff' => q{SELECT string_agg(to_table_name, ',' ORDER BY 1)
		FROM dolt_schema_diff('HEAD~3', 'HEAD')},
	'dolt_at by row type' => q{SELECT string_agg(id || '=' || v, ',' ORDER BY id)
		FROM dolt_at('HEAD~3', NULL::sh.t)},
	'dolt_at at HEAD' => q{SELECT string_agg(id || '=' || v, ',' ORDER BY id)
		FROM dolt_at('HEAD', NULL::sh.t)},
	'dolt_diff by row type' => q{SELECT string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id), ','
		ORDER BY coalesce((to_row).id, (from_row).id))
		FROM dolt_diff('HEAD~3', 'HEAD~2', NULL::sh.t)},
	'dolt_diff of the working set' => q{SELECT string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id), ','
		ORDER BY coalesce((to_row).id, (from_row).id))
		FROM dolt_diff('HEAD', 'WORKING', NULL::sh.t)},
	'dolt_diff by name' => q{SELECT string_agg(diff_type || ':' || (coalesce(to_row, from_row)->>'id'), ','
		ORDER BY (coalesce(to_row, from_row)->>'id')::int)
		FROM dolt_diff('HEAD~3', 'HEAD~2', 'sh.t')},
	'dolt_diff_table' => q{SELECT count(*) FROM dolt_diff_table('sh.t')},
	'dolt_history_table by row type' => q{SELECT count(*), count(DISTINCT commit_hash)
		FROM dolt_history_table(NULL::sh.t)},
	'dolt_history_table by name' => q{SELECT count(*) FROM dolt_history_table('sh.t')},
	'dolt_log' => q{SELECT string_agg(message, ',' ORDER BY date) FROM dolt_log},
	'dolt_status' => q{SELECT string_agg(table_name || ' ' || status, ',') FROM dolt_status},
	'a long jsonb path from history' => q{SELECT string_agg(prolly_jsonb_get(j, '{g}')::text, ',' ORDER BY 1),
			bool_and(prolly_jsonb_tree_root(j) IS NOT NULL)
		FROM dolt_at('HEAD', NULL::sh.k)},
	'a long jsonb diff from history' => q{SELECT string_agg(d.path::text || d.diff_type, ',' ORDER BY 1)
		FROM dolt_at('HEAD~1', NULL::sh.k) a JOIN dolt_at('HEAD', NULL::sh.k) b USING (v),
			prolly_jsonb_diff(a.j, b.j) d},
	'dolt_diff_summary' => q{SELECT string_agg(to_table_name || diff_type || data_change, ',' ORDER BY 1)
		FROM dolt_diff_summary('HEAD~3', 'HEAD')},
	'dolt_diff view' => q{SELECT count(*) FROM dolt_diff},
	'dolt_at of a tag and a branch' => q{SELECT (SELECT count(*) FROM dolt_at('v1', NULL::sh.k)),
		(SELECT count(*) FROM dolt_at('side', NULL::sh.t))},
	'dolt_branches and dolt_tags' => q{SELECT (SELECT string_agg(name, ',' ORDER BY name) FROM dolt_branches),
		(SELECT string_agg(tag_name, ',') FROM dolt_tags)},
	'dolt_commits and ancestors' => q{SELECT (SELECT count(*) FROM dolt_commits),
		(SELECT count(*) FROM dolt_commit_ancestors)},
	'dolt_merge_base and dolt_hashof' => q{SELECT dolt_merge_base('main', 'side') = dolt_hashof('side')},
	'dolt_reflog' => q{SELECT count(*) FROM dolt_reflog('main')},
	'dolt_conflicts' => q{SELECT count(*) FROM dolt_conflicts},
	'prolly_mounts' => q{SELECT string_agg(mount, ',') FROM prolly_mounts()},
	'history mount' => q{SELECT count(*), count(DISTINCT commit_hash) FROM hm.t},
);

for my $name (sort keys %reads)
{
	my $q = $reads{$name};
	my ($ret, $out, $err) = $standby->psql('postgres', $q);
	is($ret, 0, "$name runs on the standby") or diag($err);
	isnt($out, '', "$name answers");
	is($out, sql($q), "$name gives the primary's answer");
}

# what writes is refused as read-only, not for want of a transaction id
for my $write (
	q{SELECT dolt_branch('on_standby')},
	q{SELECT dolt_tag('on_standby')},
	q{SELECT dolt_commit('--allow-empty', '-m', 'on the standby')},
	q{SELECT dolt_reset('--hard')},
	q{SELECT prolly_refresh_mount('hm')},
	q{SELECT mount FROM prolly_mount('HEAD', 'sh', 'hm2', NULL, false, true)})
{
	my ($ret, $out, $err) = $standby->psql('postgres', $write);
	isnt($ret, 0, "$write is refused on the standby");
	like($err, qr/read-only|during recovery/, 'as read-only');
	unlike($err, qr/TransactionIds/, 'not for want of a transaction id');
}

# none of them assigned a transaction id: the standby can't
unlike(slurp_file($standby->logfile), qr/cannot assign TransactionIds during recovery/,
	'no history read asked for a transaction id');

$standby->stop;
$node->stop;
done_testing();
