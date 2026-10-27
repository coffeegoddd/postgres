# Copyright (c) 2026, PostgreSQL Global Development Group

# Pages kept as of a commit long ago, once the database has frozen past
# the transactions that wrote them and the commit-status log is truncated:
# a branch's first use doesn't make the branch of them (their xids'
# status is gone) but copies the source as it is, and a keep letting go
# of a commit retired then asks no transaction's status.  Both used to
# fail, and go on failing at every try.  The commits whose pages are kept
# but can't be given back are retired by that keep.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_old_xids');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
});
# (roots kept as captured, which keep their rows' transaction ids: frozen ones keep none)
$node->append_conf('postgresql.conf', 'prolly.freeze_roots = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION xid_wraparound; CREATE EXTENSION test_prolly');
# (a window of two commits: the third's keep retires the first's roots)
$node->safe_psql('postgres', 'ALTER DATABASE postgres SET prolly.history_min_commits = 2');

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA ko VERSIONED;
	CREATE TABLE ko.t (id int PRIMARY KEY, v int);
	INSERT INTO ko.t VALUES (1, 0);
	SELECT dolt_commit('-Am', 'c1');
	UPDATE ko.t SET v = 1;
	SELECT dolt_commit('-am', 'c2')});
# (the third commit retires c1's pages; its own rows are written in the
# transaction that commits them, so their xids are unhinted on its pages)
my $c3 = sql(q{BEGIN;
	CREATE TABLE ko.big (id int PRIMARY KEY, v int);
	INSERT INTO ko.big SELECT g, 0 FROM generate_series(1, 1000) g;
	SELECT dolt_commit('-Am', 'c3') IS NULL;
	SELECT txid_current();
	COMMIT});
$c3 =~ s/.*\n//s;
is(sql(q{SELECT test_bmap_roots_of(pg_relation_filenode('ko.t'))}),
	'2', 'ko.t kept by c1\'s snapshot (retired) and c2\'s (c3 unchanged since)');
# main changes since, and is frozen; then the database, past those xids
sql(q{INSERT INTO ko.big VALUES (1001, 1)});
sql(q{SELECT consume_xids(2500000)});
sql(q{UPDATE pg_database SET datallowconn = true WHERE datname = 'template0'});
for my $db ('postgres', 'template0', 'template1')
{
	$node->safe_psql($db, 'VACUUM (FREEZE)');
}
sql('CHECKPOINT');
sql('CHECKPOINT');
# (restarted: the status's pages are no longer in shared memory either)
$node->restart;
is(sql(qq{SELECT txid_status($c3) IS NULL}), 't',
	'the commit-status of the transaction that made c3 is gone');

# a first use at c3: made of the source's storage, not the pages kept
my ($ret, $out, $err) = $node->psql('postgres', q{
	SELECT dolt_branch('b');
	SELECT dolt_checkout('b');
	SELECT count(*) FROM ko.big WHERE v = 0;
});
is($ret, 0, 'a first use at a commit whose kept pages hold xids long frozen');
like($out, qr/1000$/, 'and the branch holds the commit\'s rows');
like($err, qr/^$/, 'without an error') or diag $err;

# a keep after: the retired commit let go of without asking c3's
# transaction's status
($ret, $out, $err) = $node->psql('postgres', q{
	UPDATE ko.t SET v = 4;
	SELECT dolt_commit('-am', 'c4') IS NOT NULL;
});
is($ret, 0, 'a commit after the retiring transaction\'s status is gone');
is(sql(q{SELECT test_bmap_roots_of(pg_relation_filenode('ko.t'))}),
	'2', 'c1\'s snapshot let go of, c4 kept by its own');
like($err, qr/^$/, 'without an error') or diag $err;
# (c3 and c2, their snapshots still there but their xids frozen past, can
# give no table its pages: retired, not counted or offered)
is(sql(q{SELECT count(*) FROM test_kept_commits('main')}), '1',
	'only c4 kept: those whose pages can\'t be given back retired');

$node->stop;
done_testing();
