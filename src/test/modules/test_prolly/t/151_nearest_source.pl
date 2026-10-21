# Copyright (c) 2026, PostgreSQL Global Development Group

# Sharing another branch's pages in a move (#491), with other sessions: a
# table another transaction uses on the moved branch isn't shared, its rows
# are written (one reading it on the other branch doesn't stop it); one
# shared is the other branch's no more once either writes it; a branch
# first used from one another session wrote since is moved by its rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('nearest_source');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
debug_prolly_verify_fold_caches = on
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	SET dolt.author = 'Ada Lovelace <ada@example.com>';
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('feat');
});

# feat: 5000 rows changed, committed, read clean
my $f = $node->background_psql('postgres');
$f->query_safe(q{SET dolt.author = 'Ada Lovelace <ada@example.com>'});
$f->query_safe(q{SELECT dolt_checkout('feat')});
$f->query_safe(q{UPDATE s.t SET v = 'feat' WHERE id <= 5000});
$f->query_safe(q{SELECT dolt_commit('-am', 'feat')});

my $a = $node->background_psql('postgres');
$a->query_safe(q{SET dolt.author = 'Ada Lovelace <ada@example.com>'});

# The rows main's move wrote to t, the move in a transaction of its own.
sub moved
{
	my ($move) = @_;
	$a->query_safe('SELECT pg_stat_force_next_flush()');
	$a->query_safe('BEGIN');
	$a->query_safe($move);
	my $n = $a->query_safe(q{
		SELECT n_tup_ins + n_tup_upd + n_tup_del FROM pg_stat_xact_user_tables
		WHERE relid = 's.t'::regclass});
	$a->query_safe('COMMIT');
	return $n;
}

sub at_feat
{
	return $a->query_safe(
		q{SELECT test_fold_root() = prolly_root_hash('feat')}) eq 't';
}

# Another transaction reading main's t: main's fast-forward writes rows.
my $r = $node->background_psql('postgres');
$f->query_safe(q{SELECT count(*) FROM dolt_status});
$r->query_safe('BEGIN');
$r->query_safe(q{SELECT count(*) FROM s.t});
is(moved(q{SELECT dolt_merge('feat')}), 5000, 'a table in use is written');
ok(at_feat(), 'at the target');
$r->query_safe('COMMIT');

# One reading feat's: shared.
$a->query_safe(q{SELECT dolt_reset('--hard', 'HEAD~1')});
$f->query_safe(q{SELECT count(*) FROM dolt_status});
$f->query_safe('BEGIN');
$f->query_safe(q{SELECT count(*) FROM s.t});
is(moved(q{SELECT dolt_reset('--hard', 'feat')}), 0, 'shared');
ok(at_feat(), 'at the target');
$f->query_safe('COMMIT');

# feat's next write is feat's alone, and main's working set stays.
$f->query_safe(q{UPDATE s.t SET v = 'feat again' WHERE id = 1});
$f->query_safe(q{SELECT dolt_commit('-am', 'again')});
is($a->query_safe(q{SELECT v FROM s.t WHERE id = 1}), 'feat', 'main unchanged');
is($a->query_safe(q{SELECT count(*) FROM dolt_status}), '0', 'main clean');
is($f->query_safe(q{SELECT v FROM s.t WHERE id = 1}), 'feat again',
	'feat\'s write');

# A branch first used from feat, which another session wrote since it was
# read: feat isn't known clean, so the branch is moved from its copies.
sql(q{SELECT dolt_branch('later', 'feat')});
$f->query_safe(q{SELECT count(*) FROM dolt_status});
$f->query_safe(q{UPDATE s.t SET v = 'unseen' WHERE id = 2});
$a->query_safe(q{SELECT dolt_checkout('later')});
is($a->query_safe(q{SELECT test_fold_root() = prolly_root_hash('HEAD')}), 't',
	'a first use at its head');
is($a->query_safe(q{SELECT v FROM s.t WHERE id = 2}), 'feat',
	'without the other branch\'s write');

$a->quit;
$f->quit;
$r->quit;
$node->stop;
done_testing();
