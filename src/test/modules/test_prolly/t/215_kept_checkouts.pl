# Copyright (c) 2026, PostgreSQL Global Development Group

# Kept materializations (a detached checkout's hidden branch kept after its
# sessions leave) with other sessions and across a restart: one a session
# is on isn't let go of past the budget; a snapshot older than a kept
# branch doesn't take its pages, which hold rows it can't see; and after a
# restart a kept branch is still a checkout's and a move's source, though
# nothing about it is known in memory any more.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_checkouts');
$node->init;
# (debug_prolly_page_roots off: the kept materializations, not pages kept as of
# a branch's head, which come before them and move nothing: t/203 and
# t/204 test those)
$node->append_conf('postgresql.conf', qq{
debug_prolly_page_roots = off
autovacuum = off
prolly_fold_worker = off
debug_prolly_verify_fold_caches = on
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# The growth of pg_stat_prolly's counter while $code ran in session $s.
sub grew
{
	my ($s, $counter, $code) = @_;
	my $q = qq{SELECT value FROM pg_stat_prolly WHERE name = '$counter'};
	$s->query_safe('SELECT pg_stat_force_next_flush()');
	my $before = $s->query_safe($q);
	$code->();
	$s->query_safe('SELECT pg_stat_force_next_flush()');
	return $s->query_safe($q) - $before;
}

sql(q{
	CREATE EXTENSION test_prolly;
	SET dolt.author = 'Ada Lovelace <ada@example.com>';
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.big (id int PRIMARY KEY, v int);
	CREATE TABLE s.small (id int PRIMARY KEY, v text);
	INSERT INTO s.big SELECT g, g FROM generate_series(1, 4000) g;
	INSERT INTO s.small VALUES (1, 'c0');
	SELECT dolt_commit('-Am', 'c0');
	UPDATE s.big SET v = v + 1 WHERE id <= 1000;
	UPDATE s.small SET v = 'c1';
	SELECT dolt_commit('-am', 'c1');
	UPDATE s.small SET v = 'c2';
	SELECT dolt_commit('-am', 'c2');
	UPDATE s.big SET v = v + 1 WHERE id > 3000;
	SELECT dolt_commit('-am', 'c3');
});
my $c1 = sql(q{SELECT commit_hash FROM dolt_log WHERE message = 'c1'});
my $c2 = sql(q{SELECT commit_hash FROM dolt_log WHERE message = 'c2'});
my $hidden1 = 'detached/' . substr($c1, 0, 40);
my $hidden2 = 'detached/' . substr($c2, 0, 40);

my $state = q{SELECT count(*) || ' ' || count(*) FILTER (WHERE v <> id) FROM s.big};

# A REPEATABLE READ transaction whose snapshot was taken before c1's kept
# branch was made: its checkout of c2 writes big's rows rather than take
# the kept branch's pages (shared, or its copies chosen whole), whose rows
# its snapshot doesn't see, and it reads c2's rows.
my $r = $node->background_psql('postgres');
$r->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$r->query_safe('SELECT count(*) FROM s.small');
my $a = $node->background_psql('postgres');
$a->query_safe(qq{SELECT dolt_checkout('$c1')});
$a->query_safe(q{SELECT dolt_checkout('main')});
is(sql(qq{SELECT count(*) FROM pg_branch WHERE brname = '$hidden1'}), '1',
	'c1 is kept');
my $shared = grew($r, 'move_tables_shared', sub {
	$r->query_safe(qq{SELECT dolt_checkout('$c2')});
});
is($shared, 0, 'an older snapshot shares nothing from it');
is($r->query_safe($state), '4000 1000', 'and reads c2\'s rows');
$r->query_safe('COMMIT');
is($r->query_safe($state), '4000 1000', 'as does its next transaction');
$r->query_safe(q{SELECT dolt_checkout('main')});

# A newer one takes them: c2 again from scratch, kept c1 seen.
sql(qq{SET prolly.kept_checkouts = 0; SELECT dolt_checkout('$c1'); SELECT dolt_checkout('main')});
sql(qq{SELECT dolt_checkout('$c1'); SELECT dolt_checkout('main')});
is(sql(q{SELECT count(*) FROM pg_branch WHERE brname LIKE 'detached/%'}), '1',
	'only c1 kept');
# (the move planner takes c1's copies, big as c2 has it: only small's row
# is written, not big's 1000)
my $written = grew($r, 'apply_rows', sub {
	$r->query_safe(qq{SELECT dolt_checkout('$c2')});
});
is($written, 1, 'a snapshot that sees it made takes its pages');
is($r->query_safe($state), '4000 1000', 'reading c2\'s rows');
$r->query_safe(q{SELECT dolt_checkout('main')});

# A session on a kept branch keeps it past the budget: with none kept,
# a sweep lets c2 go and leaves c1, which A is on.
$a->query_safe(qq{SELECT dolt_checkout('$c1')});
is(sql(qq{SET prolly.kept_checkouts = 0;
	DO \$\$BEGIN PERFORM dolt_checkout('main'); END\$\$;
	SELECT string_agg(brname, ',') FROM pg_branch WHERE brname LIKE 'detached/%'}),
	$hidden1, 'the branch a session is on stays');
is($a->query_safe($state), '4000 1000', 'its session reads c1 still');
$a->query_safe(q{SELECT dolt_checkout('main')});

# Across a restart, c1 is still kept: its checkout copies nothing, and a
# move to c2 shares big from it, though nothing of it is known in memory.
$a->quit;
$r->quit;
$node->restart;
my $b = $node->background_psql('postgres');
my $copied = grew($b, 'catalog_rows_copied', sub {
	$b->query_safe(qq{SELECT dolt_checkout('$c1')});
});
is($copied, 0, 'after a restart, a kept commit\'s checkout copies nothing');
is($b->query_safe($state), '4000 1000', 'and reads c1\'s rows');
is($b->query_safe(q{SELECT v FROM s.small}), 'c1', 'and c1\'s small');
$b->query_safe(q{SELECT dolt_checkout('main')});
$written = grew($b, 'apply_rows', sub {
	$b->query_safe(qq{SELECT dolt_checkout('$c2')});
});
is($written, 1, 'and a first use takes its pages');
is($b->query_safe($state), '4000 1000', 'reading c2\'s rows');
is($b->query_safe(q{SELECT v FROM s.small}), 'c2', 'and c2\'s small');
is($b->query_safe(q{SELECT test_fold_root() = prolly_root_hash('HEAD')}), 't',
	'at c2\'s root');
$b->query_safe(q{SELECT dolt_checkout('main')});
is($b->query_safe(q{SELECT count(*) FROM pg_branch WHERE brname LIKE 'detached/%'}),
	'2', 'both kept');

$b->quit;
$node->stop;
done_testing();
