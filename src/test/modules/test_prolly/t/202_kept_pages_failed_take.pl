# Copyright (c) 2026, PostgreSQL Global Development Group

# A keep takes its relations' snapshots together.  One that fails part way
# (an error after the first relation's snapshot is taken) leaves no
# snapshot kept by nobody: those taken already are let go of, as an
# aborted keep's are, by the session's next keep.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('kept_pages_failed_take');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');

sub kept
{
	return $node->safe_psql('postgres',
		q{SELECT test_bmap_roots_of(pg_relation_filenode('kf.t'))});
}

$node->safe_psql('postgres', q{
	CREATE SCHEMA kf VERSIONED;
	CREATE TABLE kf.t (id int PRIMARY KEY, v int);
	CREATE INDEX t_v ON kf.t (v);
	INSERT INTO kf.t SELECT g, 0 FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'c1');
});
is(kept(), '1', 'the commit keeps the table by one root');

# a commit whose keep fails after its first snapshot, then one that keeps
my ($ret, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-root-slot-captured', 'error');
	UPDATE kf.t SET v = 1 WHERE id = 1;
	SELECT dolt_commit('-am', 'c2');
	SELECT injection_points_detach('bmap-root-slot-captured');
	UPDATE kf.t SET v = 2 WHERE id = 1;
	SELECT dolt_commit('-am', 'c2 again') IS NOT NULL;
}, on_error_stop => 0);
like($err, qr/bmap-root-slot-captured/, 'the first keep failed part way');
is(kept(), '2',
	'the failed keep\'s root is reclaimed: c1\'s and c2\'s are left');

$node->stop;
done_testing();
