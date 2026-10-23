# Copyright (c) 2026, PostgreSQL Global Development Group

# A merge's exceptional paths hold no more memory as their inputs grow:
# a pass of a table's writes that is rolled back and made again releases
# the violations it recorded, the rows held for trying again go to
# temporary files, and so do the values a keyless table's removed rows
# held, once they outgrow work_mem.  Results are as before.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}
my $node = PostgreSQL::Test::Cluster->new('merge_attempts');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
work_mem = 64kB
});
$node->start;

# Bytes a context (summed over those of that name) or the backend holds,
# from the log pg_log_backend_memory_contexts() writes.
sub memory_of
{
	my ($db, $pid, $what) = @_;
	my $offset = -s $node->logfile;
	$node->safe_psql($db, "SELECT pg_log_backend_memory_contexts($pid)");
	$node->wait_for_log(qr/Grand total: \d+ bytes/, $offset);
	my $log = substr(slurp_file($node->logfile), $offset);
	my %bytes;
	($bytes{grand}) = $log =~ /Grand total: (\d+) bytes/;
	$bytes{$1} += $2 while $log =~ /level: \d+; ([^:]+): (\d+) total/g;
	return \%bytes;
}

# Wait until a backend woken up has left its wait, so that a wait seen next
# is the next one.  (A pass takes far longer than a millisecond.)
sub left_wait
{
	my ($db, $pid, $event) = @_;
	$node->safe_psql($db, qq{DO \$\$BEGIN
		WHILE EXISTS (SELECT FROM pg_stat_activity
			WHERE pid = $pid AND wait_event = '$event') LOOP
			PERFORM pg_sleep(0.001);
			PERFORM pg_stat_clear_snapshot();
		END LOOP;
	END\$\$});
}

sub temp_files
{
	my ($db, $pid) = @_;
	return $node->safe_psql($db, qq{SELECT count(*), coalesce(sum(size), 0)
		FROM pg_ls_tmpdir() WHERE name LIKE 'pgsql_tmp$pid.%'});
}

# A row whose put-back is refused twice is pinned: its table's writes are
# made four times, and in the last, every row refused is held to be tried
# again.  Each of theirs' n changes to b takes a value a row of ours holds,
# so each pass records n violations.
my (@leak, @held, @spooled);
for my $n (400, 4000)
{
	my $db = "attempts$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.t (pk int PRIMARY KEY, b int, c int, pad text);
		INSERT INTO s.t VALUES (1, 4, 1, 'p'), (2, 3, 2, 'p'), (3, 2, 4, 'p');
		INSERT INTO s.t SELECT 100 + g, 10000 + g, 20000 + g, repeat(md5(g::text), 8)
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		CREATE UNIQUE INDEX t_b ON s.t (b);
		CREATE UNIQUE INDEX t_c ON s.t (c);
		INSERT INTO s.t SELECT 100000 + g, 30000 + g, 40000 + g, 'ours'
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'ours');
		SELECT dolt_checkout('side');
		UPDATE s.t SET c = 2 WHERE pk = 1;
		UPDATE s.t SET b = 4 WHERE pk = 2;
		UPDATE s.t SET b = 30000 + pk - 100 WHERE pk > 100;
		SELECT dolt_commit('-Am', 'theirs');
		SELECT dolt_checkout('main');
	});
	my $head = $node->safe_psql($db, "SELECT dolt_hashof('HEAD')");
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-write-attempt', 'wait');
		SELECT injection_points_attach('prolly-merge-retrying-refused', 'wait')});
	$s->query_until(qr/merging/, q{\echo merging
		BEGIN;
		SELECT (dolt_merge('side'))[3:4];
	});
	my @at;
	for my $attempt (1 .. 4)
	{
		$node->wait_for_event('client backend', 'prolly-merge-write-attempt');
		push @at, memory_of($db, $pid)->{grand};
		$node->safe_psql($db, q{SELECT injection_points_wakeup('prolly-merge-write-attempt')});
		left_wait($db, $pid, 'prolly-merge-write-attempt');
	}
	$node->wait_for_event('client backend', 'prolly-merge-retrying-refused');
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-write-attempt')});
	my $m = memory_of($db, $pid);
	push @leak, $at[3] - $at[1];
	push @held, $m->{'merge rows held'};
	my ($files, $bytes) = split /\|/, temp_files($db, $pid);
	push @spooled, $bytes;
	cmp_ok($at[3] - $at[1], '<', 64 * 1024,
		"$n rows: passes rolled back release their violations");
	cmp_ok($m->{'merge rows held'}, '<', 128 * 1024,
		"$n rows: rows held for trying again stay below 128kB");
	cmp_ok($files, '>=', 1, "$n rows: the rows held are on disk");
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-retrying-refused');
		SELECT injection_points_wakeup('prolly-merge-retrying-refused')});
	$s->query_safe('SELECT 1');
	is($s->query_safe(q{SELECT count(*) FROM dolt_constraint_violations('s.t')}),
		$n + 2, "$n rows: theirs' changes refused are recorded once each");
	is($s->query_safe(q{SELECT string_agg(format('%s:%s:%s', pk, b, c), ' ' ORDER BY pk)
		FROM s.t WHERE pk < 100}), '1:4:1 2:3:2 3:2:4',
		"$n rows: the rows refused stay as the branch has them");
	is($s->query_safe(qq{SELECT count(*) FROM s.t
		WHERE pk > 100 AND pk < 100000 AND b = 10000 + pk - 100}), $n,
		"$n rows: each row refused is put back");
	is(temp_files($db, $pid), '0|0', "$n rows: the merge removes its files");
	$s->query_safe('ROLLBACK');

	# Canceled while rows are held, the files go too.
	$s->query_safe(q{SELECT injection_points_attach('prolly-merge-retrying-refused', 'wait')});
	$s->query_until(qr/canceling/, q{\echo canceling
		BEGIN;
		SELECT dolt_merge('side');
	});
	$node->wait_for_event('client backend', 'prolly-merge-retrying-refused');
	$node->safe_psql($db, "SELECT pg_cancel_backend($pid)");
	$s->query('ROLLBACK');
	like($s->{stderr}, qr/canceling statement due to user request/,
		"$n rows: the merge was canceled");
	$s->{stderr} = '';
	is($s->query_safe("SELECT dolt_hashof('HEAD')"), $head,
		"$n rows: cancellation leaves the head unchanged");
	is(temp_files($db, $pid), '0|0', "$n rows: cancellation removes the files");
	$s->query_safe(q{SELECT injection_points_detach('prolly-merge-retrying-refused')});
	$s->quit;
}
cmp_ok($held[1], '<=', $held[0] + 16384,
	'ten times as many rows held do not take ten times the memory');
cmp_ok($spooled[1], '>', $spooled[0] * 5, 'the rows held go to temporary files');
note("memory left by passes rolled back: @leak bytes; rows held: @held bytes in memory, @spooled bytes on disk");

# The rows refused while a row is pinned are tried again once it's updated,
# and those that fit then are written, their violations forgotten.  Row 3's
# new version needs the c row 2's takes, and its put-back the b row 2's
# takes; row 2's put-back needs the b row 1's takes: so row 2 and then row 3
# are ranked up, row 3 pinned.  In the pinned pass, row 3's old version
# holds c = 300 until it is updated, last, and theirs' n other rows form a
# chain, the first taking 300 and each next the c the one before it gives
# up: each is refused, put back, and written when tried again.  Only rows 1
# and 2 keep violations.
for my $n (500, 5000)
{
	my $db = "retried$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.a (pk int PRIMARY KEY, b int, c int);
		INSERT INTO s.a VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300);
		INSERT INTO s.a SELECT 100 + g, 1000 + g, 100000 + g
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		CREATE UNIQUE INDEX a_b ON s.a (b);
		CREATE UNIQUE INDEX a_c ON s.a (c);
		SELECT dolt_commit('-Am', 'ours');
		SELECT dolt_checkout('side');
		UPDATE s.a SET b = 20 WHERE pk = 1;
		UPDATE s.a SET b = 30, c = 999 WHERE pk = 2;
		UPDATE s.a SET b = 77, c = 999 WHERE pk = 3;
		UPDATE s.a SET c = CASE pk WHEN 101 THEN 300 ELSE 100000 + pk - 101 END
		  WHERE pk > 100;
		SELECT dolt_commit('-Am', 'theirs');
		SELECT dolt_checkout('main');
	});
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-retrying-refused', 'wait')});
	$s->query_until(qr/merging/, q{\echo merging
		BEGIN;
		SELECT (dolt_merge('side'))[3:4];
	});
	$node->wait_for_event('client backend', 'prolly-merge-retrying-refused');
	my ($files, $bytes) = split /\|/, temp_files($db, $pid);
	cmp_ok($bytes, '>', $n * 20, "$n rows: the rows to try again are on disk");
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-retrying-refused');
		SELECT injection_points_wakeup('prolly-merge-retrying-refused')});
	$s->query_safe('SELECT 1');
	is($s->query_safe(q{SELECT string_agg(violating_row->>'pk', ' ' ORDER BY violating_row->>'pk')
		FROM dolt_constraint_violations('s.a')}), '1 2',
		"$n rows: only the rows still refused have violations");
	is($s->query_safe(q{SELECT string_agg(format('%s:%s:%s', pk, b, c), ' ' ORDER BY pk)
		FROM s.a WHERE pk < 100}), '1:10:100 2:20:200 3:77:999',
		"$n rows: the pinned row is written, the others put back");
	is($s->query_safe(qq{SELECT count(*) FROM s.a WHERE pk > 100 AND
		c = CASE pk WHEN 101 THEN 300 ELSE 100000 + pk - 101 END}), $n,
		"$n rows: each row tried again is written");
	is(temp_files($db, $pid), '0|0', "$n rows: the merge removes its files");
	$s->query_safe('ROLLBACK');
	$s->quit;
}

# A keyless table's rows removed hold values a NOT VALID domain constraint
# allows; theirs' changed rows insert them again, which isn't checked.
my (@removed, @removed_disk, @in_memory);
for my $n (20, 3000)
{
	my $db = "removed$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE DOMAIN s.short AS text;
		CREATE TABLE s.k (v s.short, w int);
		INSERT INTO s.k SELECT repeat(md5(g::text), 16), g FROM generate_series(1, $n) g;
		INSERT INTO s.k VALUES (NULL, 0);
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		ALTER DOMAIN s.short ADD CONSTRAINT short CHECK (length(VALUE) < 10) NOT VALID;
		SELECT dolt_commit('-Am', 'ours: short, not valid');
		SELECT dolt_checkout('side');
		UPDATE s.k SET w = -w;
		SELECT dolt_commit('-Am', 'theirs: w');
		SELECT dolt_checkout('main');
	});
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-removed-values-held', 'wait')});
	$s->query_until(qr/merging/, q{\echo merging
		BEGIN;
		SELECT (dolt_merge('side'))[3:4];
	});
	$node->wait_for_event('client backend', 'prolly-merge-removed-values-held');
	my $m = memory_of($db, $pid);
	my $values = $m->{'merge removed values'} + ($m->{'merge removed values held'} // 0);
	push @removed, $values;
	push @in_memory, exists $m->{'merge removed values held'} ? 1 : 0;
	my ($files, $bytes) = split /\|/, temp_files($db, $pid);
	push @removed_disk, $bytes;
	cmp_ok($values, '<', 256 * 1024, "$n rows: removed values stay below 256kB");
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-removed-values-held');
		SELECT injection_points_wakeup('prolly-merge-removed-values-held')});
	$s->query_safe('SELECT 1');
	is($s->query_safe(q{SELECT count(*) FROM dolt_constraint_violations()}), 0,
		"$n rows: values removed and inserted again aren't refused");
	is($s->query_safe(q{SELECT count(*) FROM s.k WHERE w < 0 OR v IS NULL}), $n + 1,
		"$n rows: theirs' changes are written");
	is(temp_files($db, $pid), '0|0', "$n rows: the merge removes its files");
	$s->query_safe('ROLLBACK');

	# A value no row removed held is checked, spilled or not.
	$node->safe_psql($db, q{SELECT dolt_checkout('side');
		UPDATE s.k SET v = v || 'x' WHERE w = -1;
		SELECT dolt_commit('-am', 'theirs: a new value');
		SELECT dolt_checkout('main')});
	$s->query(q{SELECT dolt_merge('side')});
	like($s->{stderr}, qr/rows of keyless table .* can't be merged/,
		"$n rows: a new value breaking the domain is refused");
	$s->{stderr} = '';
	$s->quit;
}
is("@in_memory", '1 0', 'removed values are held in memory only while they fit');
cmp_ok($removed_disk[1], '>', $removed_disk[0] + 3000 * 256,
	'removed values past work_mem go to temporary files');
note("removed values: @removed bytes in memory, @removed_disk bytes on disk");
$node->stop;
done_testing();
