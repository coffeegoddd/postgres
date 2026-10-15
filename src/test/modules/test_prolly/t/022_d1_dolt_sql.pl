# Copyright (c) 2026, PostgreSQL Global Development Group

# Doltgres's SQL interface to version control: the dolt_* functions with
# Dolt's arguments, the dolt_log, dolt_branches and dolt_status views,
# dolt_diff, the dolt.author setting, and "dbname/branch" connections.
# Merges fast-forward when they can; a hard reset drops the changes since
# a commit and survives a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('dolt_sql');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

sub fails
{
	my ($query, $re, $name, $db) = @_;
	my ($ret, $out, $err) = $node->psql($db // 'postgres', $query);
	like($err, $re, $name);
}

# Commits and their authors.
is(sql(q{SELECT count(*) FROM dolt_log}), '0', 'no commits to start with');
sql(q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t VALUES (1, 'a'), (2, 'b');
	CREATE TABLE k (x int);
	INSERT INTO k VALUES (1), (1);
});
like(sql(q{SET dolt.author = 'Ada <ada@example.com>';
	SELECT dolt_add('-A'), dolt_commit('-Am', 'first')}),
	qr/^\{0\}\|\{[0-9a-f]{64}\}$/, 'dolt_commit returns the commit id');
fails(q{SELECT dolt_commit('-m', 'again')}, qr/nothing to commit/,
	'a commit with nothing changed is refused');
sql(q{SET dolt.author = 'Ada <ada@example.com>';
	SELECT dolt_commit('-m', 'empty', '--allow-empty')});
fails(q{SELECT dolt_commit('-a')}, qr/dolt_commit needs a message/,
	'a commit needs a message');
fails(q{SELECT dolt_commit('-m', 'x', '--amend')},
	qr/dolt_commit: "--amend" is not supported/, 'unsupported options are refused');
is(sql(q{SELECT string_agg(committer || ':' || email || ':' || message || ':' || commit_order, ','
	ORDER BY commit_order DESC) FROM dolt_log}),
	'Ada:ada@example.com:empty:2,Ada:ada@example.com:first:1',
	'dolt_log lists the commits, newest first, with dolt.author');
my $user = sql(q{SELECT current_user});
sql(q{INSERT INTO t VALUES (9, 'z'); SELECT dolt_commit('-m', 'no author set')});
is(sql(q{SELECT committer || ':' || email FROM dolt_log WHERE commit_order = 3}),
	"$user:", 'without dolt.author, commits are by the session user');
is(sql(q{SELECT dolt_hashof('HEAD~2')}),
	sql(q{SELECT commit_hash FROM dolt_log WHERE message = 'first'}),
	'HEAD~2 is the first commit');
sql(q{DELETE FROM t WHERE id = 9; SELECT dolt_commit('-m', 'drop 9')});

# Branches, and connections to them.
is(sql(q{SELECT dolt_checkout('-b', 'feature'); SELECT active_branch()}),
	"{0,\"Switched to branch 'feature'\"}\nfeature", 'checkout -b makes and checks out a branch');
is(sql(q{SELECT active_branch()}), 'main', 'a new session is on main');
is(sql(q{SELECT active_branch()}, 'postgres/feature'), 'feature',
	'connecting to dbname/branch checks the branch out');
fails(q{SELECT 1}, qr/prolly branch "nope" does not exist/,
	'connecting to a branch that doesn\'t exist fails', 'postgres/nope');
sql(q{
	UPDATE t SET v = 'B' WHERE id = 2;
	INSERT INTO t VALUES (3, repeat('c', 5000));
	DELETE FROM t WHERE id = 1;
	INSERT INTO k VALUES (1);
	DELETE FROM k WHERE ctid = (SELECT min(ctid) FROM k);
	INSERT INTO k VALUES (2);
}, 'postgres/feature');
is(sql(q{SELECT string_agg(table_name || ':' || staged || ':' || status, ',') FROM dolt_status},
	'postgres/feature'), 'public.k:true:modified,public.t:true:modified',
	'dolt_status lists the tables changed since the head commit');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'main has no changes');
is(sql(q{SELECT string_agg(diff_type || ':' || coalesce(from_row::text, '-') || ':' ||
	coalesce(left(to_row::text, 8), '-'), ',' ORDER BY diff_type)
	FROM dolt_diff('HEAD', 'WORKING', NULL::t)}, 'postgres/feature'),
	'added:-:(3,ccccc,modified:(2,b):(2,B),removed:(1,a):-',
	'dolt_diff gives each changed row as it was and is, large values included');
is(sql(q{SELECT string_agg(diff_type || ':' || coalesce(from_row::text, '-') || ':' ||
	coalesce(to_row::text, '-'), ',' ORDER BY diff_type)
	FROM dolt_diff('HEAD', 'WORKING', NULL::k)}, 'postgres/feature'),
	'added:-:(2)',
	'a keyless table\'s copies are diffed by count');
is(sql(q{SELECT name || ':' || dirty FROM dolt_branches ORDER BY name}),
	"feature:true\nmain:false", 'dolt_branches shows which branches have changes');
sql(q{SELECT dolt_commit('-m', 'feature work', '--author', 'Bob <bob@example.com>')},
	'postgres/feature');
fails(q{SELECT dolt_diff('HEAD', 'WORKING', NULL::int)},
	qr/dolt_diff needs a table's row type/, 'dolt_diff needs a row type');

# Merges.
my $feature = sql(q{SELECT dolt_hashof('feature')});
is(sql(q{SELECT dolt_merge('feature')}),
	"{$feature,1,0,\"merge successful\"}", 'a merge fast-forwards when it can');
is(sql(q{SELECT string_agg(id || ':' || left(v, 1), ',' ORDER BY id) FROM t}),
	'2:B,3:c', 'and its rows are the branch\'s');
is(sql(q{SELECT dolt_merge('feature')}), "{$feature,0,0,\"Everything up-to-date\"}",
	'merging again changes nothing');
sql(q{SELECT dolt_checkout('-b', 'other'); INSERT INTO t VALUES (4, 'd');
	SELECT dolt_commit('-m', 'other work')});
sql(q{INSERT INTO t VALUES (5, 'e'); SELECT dolt_commit('-m', 'main work')});
my $merge = sql(q{SELECT dolt_merge('other', '-m', 'bring in other')});
like($merge, qr/^\{[0-9a-f]{64},0,0,"merge successful"\}$/, 'a merge commit');
is(sql(q{SELECT message || ':' || commit_order FROM dolt_log ORDER BY commit_order DESC LIMIT 1}),
	'bring in other:7', 'with its message, after both parents');
is(sql(q{SELECT dolt_hashof('HEAD^2')}), sql(q{SELECT dolt_hashof('other')}),
	'HEAD^2 is the merged branch\'s head');
sql(q{SELECT dolt_checkout('-b', 'ff'); INSERT INTO t VALUES (6, 'f');
	SELECT dolt_commit('-m', 'ff work'); SELECT dolt_checkout('main')});
like(sql(q{SELECT dolt_merge('--no-ff', 'ff')}), qr/^\{[0-9a-f]{64},0,0,"merge successful"\}$/,
	'--no-ff makes a merge commit');
isnt(sql(q{SELECT dolt_hashof('HEAD')}), sql(q{SELECT dolt_hashof('ff')}),
	'not the branch\'s head');

# Hard resets.
my $rows = sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t});
sql(q{UPDATE t SET v = 'oops'; DELETE FROM t WHERE id = 2});
is(sql(q{SELECT dolt_reset('--hard')}), '{0}', 'dolt_reset --hard');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t WHERE v <> 'oops'}), $rows,
	'drops the changes since the head commit');
fails(q{SELECT dolt_reset('HEAD~1')}, qr/dolt_reset only does hard resets/,
	'only hard resets');
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}), '2,3,4,5',
	'a reset to an earlier commit');
$node->stop('immediate');
$node->start;
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}), '2,3,4,5',
	'survives a crash');
is(sql(q{SELECT message FROM dolt_log LIMIT 1}), 'bring in other', 'and so does the head');

# Branch management.
sql(q{SELECT dolt_branch('-c', 'other', 'copy')});
is(sql(q{SELECT dolt_hashof('copy') = dolt_hashof('other')}), 't', 'dolt_branch -c copies a branch');
sql(q{SELECT dolt_branch('-d', 'copy', 'ff')});
is(sql(q{SELECT string_agg(name, ',' ORDER BY name) FROM dolt_branches}), 'feature,main,other',
	'dolt_branch -d deletes branches');
fails(q{SELECT dolt_branch('bad~name')}, qr/invalid prolly branch name/,
	'branch names can\'t hold ancestry steps');
fails(q{SELECT dolt_branch('-d', 'main')}, qr/cannot delete prolly branch "main"/,
	'main can\'t be deleted');

$node->stop;
done_testing();
