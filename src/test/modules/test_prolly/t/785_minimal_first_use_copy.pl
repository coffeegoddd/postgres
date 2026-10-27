# Copyright (c) 2026, PostgreSQL Global Development Group

# Under wal_level = minimal, a branch's first use at a commit with no root
# of its own (c1, between roots) begins its tables empty, in new storage
# that skips WAL and is synced at commit, then gives some of them storage
# shared with a root's pages, which is logged and has no pending sync.
# The relation cache must say so: else writes to that storage, the rows
# applied after, skip WAL with nothing to sync them at commit (and an
# assertion trips at commit, its view disagreeing with the pending
# syncs').  The first use commits, and a crash right after loses nothing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('minimal_first_use_copy');
$node->init;
$node->append_conf('postgresql.conf', q{
wal_level = minimal
max_wal_senders = 0
autovacuum = off
prolly_fold_worker = off
prolly.root_interval = '1h'
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA fu VERSIONED;
	CREATE TABLE fu.t (id int PRIMARY KEY, v text NOT NULL);
	CREATE INDEX t_v ON fu.t (left(v, 20));
	INSERT INTO fu.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'c0')});
# (c0's root the hour's; c1 and c2 have none)
# (c1's rows toasted, so its first use writes the TOAST table and index)
sql(q{UPDATE fu.t SET v = (SELECT string_agg(md5(id::text || g::text), '') FROM generate_series(1, 200) g)
	WHERE id % 7 = 0; SELECT dolt_commit('-am', 'c1')});
my $c1 = sql(q{SELECT commit_hash FROM dolt_log() LIMIT 1});
my $at_c1 = sql(q{SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM fu.t t});
sql(q{UPDATE fu.t SET v = 'main' WHERE id <= 500; SELECT dolt_commit('-am', 'c2')});
# (made at c1 once main has moved on, never used)
sql(qq{SELECT dolt_branch('b', '$c1')});

# the first use of b: its tables copied, in a transaction of their own
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT dolt_checkout('b') IS NOT NULL});
is($ret, 0, 'the first use of b commits');
diag($err) if $ret != 0;

$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{SELECT dolt_checkout('b') IS NOT NULL;
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM fu.t t}), "t\n$at_c1",
	'after a crash, b has c1\'s rows');
is($node->safe_psql('postgres', q{SELECT dolt_checkout('b') IS NOT NULL;
	SET enable_seqscan = off; SELECT count(*) FROM fu.t WHERE left(v, 20) LIKE 'v%'}), "t\n1715",
	'and its index finds them');

$node->stop;
done_testing();
