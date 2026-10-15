# Copyright (c) 2026, PostgreSQL Global Development Group

# Merge conflicts, as in Dolt.  A merge applies every change that doesn't
# conflict and leaves the merge in progress with the rest: dolt_conflicts
# lists them, each row as the ancestor, this branch and the merged branch
# have it; dolt_conflicts_resolve keeps ours or takes theirs; dolt_commit
# then makes the merge commit.  A transaction can't commit with unresolved
# conflicts unless dolt.allow_commit_conflicts is on, in which case they
# stay (through a crash) until resolved or the merge is aborted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('conflicts');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub fails
{
	my ($query, $re, $name) = @_;
	my ($ret, $out, $err) = $node->psql('postgres', $query);
	like($err, $re, $name);
}

sub rows
{
	return sql(qq{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM $_[0]});
}

sql(q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	CREATE INDEX t_v ON t (v);
	INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 6) g;
	CREATE TABLE u (id int PRIMARY KEY, v text);
	INSERT INTO u VALUES (1, 'u1');
	SELECT dolt_commit('-m', 'base');
	SELECT dolt_branch('other');
	UPDATE t SET v = 'main1' WHERE id = 1;
	UPDATE t SET v = 'main2' WHERE id = 2;
	UPDATE t SET v = 'same' WHERE id = 4;
	INSERT INTO t VALUES (7, 'main7');
	UPDATE u SET v = 'main' WHERE id = 1;
	SELECT dolt_commit('-m', 'main');
	SELECT dolt_checkout('other');
	UPDATE t SET v = 'other1' WHERE id = 1;
	DELETE FROM t WHERE id = 2;
	UPDATE t SET v = 'other3' WHERE id = 3;
	UPDATE t SET v = 'same' WHERE id = 4;
	INSERT INTO t VALUES (7, 'other7');
	UPDATE u SET v = 'other' WHERE id = 1;
	SELECT dolt_commit('-m', 'other');
});
my $main = rows('t');

# Outside a transaction block, conflicts roll the merge back.
fails(q{SELECT dolt_merge('other')}, qr/merge conflict detected, transaction rolled back/,
	'outside a transaction block, a merge with conflicts fails');
is(rows('t'), $main, 'and changes nothing');

# In one, they can be looked at, but not committed.
my $listing = sql(q{
	BEGIN;
	SELECT dolt_merge('other');
	SELECT string_agg(table_name || ':' || num_conflicts, ',' ORDER BY table_name) FROM dolt_conflicts;
	SELECT string_agg(coalesce(base_row::text, '-') || '|' || coalesce(our_row::text, '-') || '|' ||
		coalesce(their_row::text, '-') || '|' || our_diff_type || '|' || their_diff_type, ','
		ORDER BY coalesce(our_row, their_row))
		FROM dolt_conflicts(NULL::t);
	SELECT string_agg(table_name || ':' || staged || ':' || status, ',' ORDER BY table_name) FROM dolt_status;
	SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM t;
	ROLLBACK;
});
is($listing, join("\n",
	'{"",0,1,"conflicts found"}',
	'public.t:3,public.u:1',
	'(1,v1)|(1,main1)|(1,other1)|modified|modified,(2,v2)|(2,main2)|-|modified|removed,'
	  . '-|(7,main7)|(7,other7)|added|added',
	'public.t:false:conflict,public.u:false:conflict',
	'1:main1,2:main2,3:other3,4:same,5:v5,6:v6,7:main7'),
	'conflicts are listed by table and row; everything else is merged');
fails(q{BEGIN; SELECT dolt_merge('other'); COMMIT;},
	qr/cannot commit a transaction with unresolved merge conflicts/,
	'a transaction with unresolved conflicts can\'t commit');
fails(q{BEGIN; SELECT dolt_merge('other'); SELECT dolt_commit('-m', 'x');},
	qr/cannot commit with unresolved merge conflicts/,
	'nor can dolt_commit');
is(rows('t'), $main, 'nothing was merged');

# Resolved: theirs for t, ours for u; the merge commit has both parents.
sql(q{
	BEGIN;
	SELECT dolt_merge('other');
	SELECT dolt_conflicts_resolve('--theirs', 't');
	SELECT dolt_conflicts_resolve('--ours', 'u');
	SELECT dolt_commit('-m', 'merge other');
	COMMIT;
});
is(rows('t'), '1:other1,3:other3,4:same,5:v5,6:v6,7:other7', 'taking theirs makes their rows');
is(rows('u'), '1:main', 'keeping ours keeps ours');
is(sql(q{SELECT dolt_hashof('HEAD^2') = dolt_hashof('other')}), 't',
	'the merge commit\'s second parent is the merged head');
is(sql(q{SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM t
	WHERE v IN ('other1', 'main1', 'main2', 'other7')}), '1,7', 'the btree index is right');
is(sql(q{SELECT count(*) FROM dolt_conflicts}), '0', 'no conflicts remain');
is(sql(q{SELECT dolt_merge('other')}) =~ /Everything up-to-date/ ? 1 : 0, 1,
	'the branch is merged');

# Kept with dolt.allow_commit_conflicts, through a crash, then aborted.
sql(q{
	SELECT dolt_checkout('other');
	UPDATE t SET v = 'again' WHERE id = 5;
	SELECT dolt_commit('-m', 'other again');
	SELECT dolt_checkout('main');
	UPDATE t SET v = 'main again' WHERE id = 5;
	UPDATE t SET v = 'kept' WHERE id = 6;
	SELECT dolt_commit('-m', 'main again');
});
my $before = rows('t');
is(sql(q{SET dolt.allow_commit_conflicts = on; SELECT dolt_merge('other')}),
	'{"",0,1,"conflicts found"}', 'with dolt.allow_commit_conflicts, conflicts are kept');
$node->stop('immediate');
$node->start;
is(sql(q{SELECT table_name || ':' || num_conflicts FROM dolt_conflicts}), 'public.t:1',
	'through a crash');
fails(q{SELECT dolt_merge('other')}, qr/a merge is in progress/,
	'another merge waits for this one');
is(sql(q{SELECT dolt_merge('--abort')}), '{"",0,0,"merge aborted"}', 'dolt_merge --abort');
is(rows('t'), $before, 'puts the rows back');
is(sql(q{SELECT count(*) FROM dolt_conflicts}), '0', 'and ends the merge');
fails(q{SELECT dolt_conflicts_resolve('--ours', 't')}, qr/table "t" has no merge conflicts/,
	'there is nothing left to resolve');

# prolly_merge() still fails on conflicts, changing nothing.
fails(q{SELECT prolly_merge('other')}, qr/prolly merge of branch "other" conflicts in table "t"/,
	'prolly_merge() fails on conflicts');
is(rows('t'), $before, 'changing nothing');

$node->stop;
done_testing();
