# Copyright (c) 2026, PostgreSQL Global Development Group

# Diff and three-way merge of prolly branches.  A merge applies the other
# branch's row changes as ordinary writes, so indexes of every kind stay
# right; rows both branches changed alike merge, rows changed differently
# are a conflict that changes nothing; copies of a keyless table's rows add
# up; large values merge; a branch with changes since its head commit, or
# a merge already done, is refused or a no-op; and the result survives a
# crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('merge');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE TABLE m (id int PRIMARY KEY, v text, big text);
	CREATE INDEX m_v ON m USING btree (v);
	CREATE UNIQUE INDEX m_big ON m USING prolly_d1_index (left(big, 20));
	INSERT INTO m SELECT g, 'v' || g, g || repeat('x', 3000) FROM generate_series(1, 10) g;
	CREATE TABLE k (a int, b int);
	INSERT INTO k VALUES (1, 1), (1, 1), (2, 2);
	SELECT prolly_commit('base');
	SELECT prolly_branch_create('f', NULL);
});

# Changes on both branches: some alike, none conflicting.
sql(q{
	SELECT prolly_checkout('f');
	UPDATE m SET v = 'f' || id WHERE id <= 3;
	UPDATE m SET big = 'F' || big WHERE id = 9;
	DELETE FROM m WHERE id = 10;
	INSERT INTO m VALUES (11, 'v11', '11' || repeat('y', 4000));
	UPDATE m SET v = 'same' WHERE id = 5;
	INSERT INTO k VALUES (1, 1), (3, 3);
	SELECT prolly_commit('f');
	SELECT prolly_checkout('main');
	UPDATE m SET v = 'main' WHERE id = 4;
	UPDATE m SET v = 'same' WHERE id = 5;
	DELETE FROM k WHERE a = 2;
	INSERT INTO k VALUES (1, 1);
	SELECT prolly_commit('main');
});
is(sql(q{SELECT string_agg(change, ',' ORDER BY change) FROM prolly_diff('HEAD', 'f')
	WHERE relation = 'm'::regclass}),
	'added,modified,modified,modified,modified,modified,removed',
	'diff reports each changed row once, by kind');
my $merge = sql(q{SELECT prolly_merge('f')});
ok($merge ne '', 'the merge commits');
is(sql(q{SELECT string_agg(id || ':' || v || ':' || left(big, 3), ',' ORDER BY id) FROM m}),
	'1:f1:1xx,2:f2:2xx,3:f3:3xx,4:main:4xx,5:same:5xx,6:v6:6xx,7:v7:7xx,8:v8:8xx,9:v9:F9x,11:v11:11y',
	'both branches\' changes are merged, large values included');
is(sql(q{SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM m
	WHERE v IN ('f1', 'main', 'v11')}), '1,4,11', 'the btree index is right');
is(sql(q{SET enable_seqscan = off; SELECT id FROM m WHERE left(big, 20) = left('11' || repeat('y', 4000), 20)}),
	'11', 'the prolly index is right');
is(sql(q{SELECT string_agg(a || ':' || b, ',' ORDER BY a, b) FROM k}),
	'1:1,1:1,1:1,1:1,3:3', 'copies of keyless rows add up');
is(sql(q{SELECT prolly_merge('f')}), $merge, 'merging again changes nothing');

# A conflict changes nothing.
sql(q{
	SELECT prolly_branch_create('g', NULL);
	SELECT prolly_checkout('g');
	UPDATE m SET v = 'g' WHERE id = 6;
	SELECT prolly_commit('g');
	SELECT prolly_checkout('main');
	UPDATE m SET v = 'main6' WHERE id = 6;
	SELECT prolly_commit('main6');
});
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT prolly_merge('g')});
like($err, qr/prolly merge of branch "g" conflicts in table "m"/,
	'rows changed differently on both branches conflict');
is(sql(q{SELECT v FROM m WHERE id = 6}), 'main6', 'a conflict changes nothing');

# Changes since the head commit are refused.
sql(q{INSERT INTO m VALUES (12, 'dirty', '12')});
($ret, $out, $err) = $node->psql('postgres', q{SELECT prolly_merge('g')});
like($err, qr/has changes since its last commit/,
	'a branch with changes since its head commit isn\'t merged into');

# Through a crash.
my $rows = sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM m});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM m}), $rows,
	'the merged rows survive a crash');

$node->stop;
done_testing();
