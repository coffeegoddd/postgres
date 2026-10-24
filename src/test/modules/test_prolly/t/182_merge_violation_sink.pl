# Copyright (c) 2026, PostgreSQL Global Development Group

# The violations a merge records are held in a sort that spills past
# work_mem, deduplicated there against those the branch has, and added to
# the branch's after them, in batches: memory doesn't grow with how many
# there are, nor with how many the branch has already.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}
my $node = PostgreSQL::Test::Cluster->new('merge_violations');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
work_mem = 64kB
dolt.allow_commit_conflicts = on
});
$node->start;

# Bytes a context and those under it hold (summed over those of that name),
# from the log pg_log_backend_memory_contexts() writes.
sub memory_of
{
	my ($db, $pid) = @_;
	my $offset = -s $node->logfile;
	$node->safe_psql($db, "SELECT pg_log_backend_memory_contexts($pid)");
	$node->wait_for_log(qr/Grand total: \d+ bytes/, $offset);
	my $log = substr(slurp_file($node->logfile), $offset);
	my %bytes;
	# (each context's bytes with those of the contexts under it)
	my @open;
	while ($log =~ /level: (\d+); ([^:]+): (\d+) total/g)
	{
		my ($level, $name, $total) = ($1, $2, $3);
		@open = grep { $_->[0] < $level } @open;
		$bytes{ $_->[1] } += $total for @open;
		$bytes{$name} += $total;
		push @open, [ $level, $name ];
	}
	return \%bytes;
}

# Bytes the statement's contexts hold: its portal's and its transaction's,
# with those under them, but for the chunk store's caches ("prolly ...").
sub statement_memory
{
	my ($db, $pid) = @_;
	my $offset = -s $node->logfile;
	$node->safe_psql($db, "SELECT pg_log_backend_memory_contexts($pid)");
	$node->wait_for_log(qr/Grand total: \d+ bytes/, $offset);
	my $log = substr(slurp_file($node->logfile), $offset);
	my ($bytes, $under) = (0, undef);
	while ($log =~ /level: (\d+); ([^:]+): (\d+) total/g)
	{
		my ($level, $name, $total) = ($1, $2, $3);
		$under = undef if defined $under && $level <= $under;
		$under = $level
		  if !defined $under
		  && ($name eq 'TopPortalContext' || $name eq 'TopTransactionContext');
		$bytes += $total if defined $under && $name !~ /^prolly /;
	}
	return $bytes;
}

sub temp_bytes
{
	my ($db, $pid) = @_;
	return $node->safe_psql($db, qq{SELECT coalesce(sum(size), 0)
		FROM pg_ls_tmpdir() WHERE name LIKE 'pgsql_tmp$pid.%'});
}

# Theirs' changes to b take values rows of ours hold: each is refused and
# recorded.  A second branch makes the same changes again (the same
# violations, which the branch has by then) and inserts n rows refused too.
my (@first, @second, @adding, @statement, @disk);
for my $n (5000, 50000)
{
	my $db = "violations$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.t (pk int PRIMARY KEY, b int, pad text);
		INSERT INTO s.t SELECT g, 1000000 + g, repeat(md5(g::text), 8)
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		SELECT dolt_branch('again');
		CREATE UNIQUE INDEX t_b ON s.t (b);
		INSERT INTO s.t SELECT 10000000 + g, 3000000 + g, 'ours'
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'ours');
		SELECT dolt_checkout('side');
		UPDATE s.t SET b = 3000000 + pk;
		SELECT dolt_commit('-Am', 'theirs');
		SELECT dolt_checkout('again');
		UPDATE s.t SET b = 3000000 + pk;
		INSERT INTO s.t SELECT 20000000 + g, 3000000 + g, 'again'
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'again');
		SELECT dolt_checkout('main');
	});
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-publishing-violations', 'wait');
		SELECT injection_points_attach('prolly-merge-violations-added', 'wait')});
	$s->query_until(qr/merging/, q{\echo merging
		SELECT dolt_merge('side');
	});
	$node->wait_for_event('client backend', 'prolly-merge-publishing-violations');
	my $m = memory_of($db, $pid);
	push @first, $m->{'merge violations'};
	push @disk, temp_bytes($db, $pid);
	cmp_ok($m->{'merge violations'}, '<', 512 * 1024,
		"$n violations: the merge holds less than 512kB of them");
	$node->safe_psql($db, q{SELECT injection_points_wakeup('prolly-merge-publishing-violations')});
	# Publishing: the identity sort read back into the found-order sort, a
	# batch of them added to the branch's.
	$node->wait_for_event('client backend', 'prolly-merge-violations-added');
	$m = memory_of($db, $pid);
	push @adding, $m->{'merge violations'};
	cmp_ok($m->{'merge violations'}, '<', 1536 * 1024,
		"$n violations: publishing them holds less than 1.5MB");
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-violations-added');
		SELECT injection_points_wakeup('prolly-merge-violations-added')});
	$s->query_safe('SELECT 1');
	# (each refused row, and the row of ours' it collides with, as Dolt
	# records them)
	is($s->query_safe(q{SELECT count(*) FROM dolt_constraint_violations('s.t')}),
		2 * $n, "$n violations: each is recorded");
	is($s->query_safe(q{SELECT count(DISTINCT violating_row) FROM dolt_constraint_violations('s.t')}),
		2 * $n, "$n violations: each once");
	is($s->query_safe(qq{SELECT count(*) FROM s.t WHERE pk <= $n AND b = 1000000 + pk}),
		$n, "$n violations: the rows refused stay as the branch has them");

	$s->query_safe(q{SELECT dolt_commit('-afm', 'merged with violations')});
	is($s->query_safe(q{SELECT count(*) FROM dolt_constraint_violations('s.t')}),
		2 * $n, "$n violations: committed, the branch keeps them");

	# Merged again, the branch's violations are found again and kept once;
	# the new ones follow them, in the order found.
	$s->query_until(qr/again/, q{\echo again
		SELECT dolt_merge('again');
	});
	$node->wait_for_event('client backend', 'prolly-merge-publishing-violations');
	$m = memory_of($db, $pid);
	push @second, $m->{'merge violations'};
	# (nor does the statement count the branch's violations by loading them)
	push @statement, statement_memory($db, $pid);
	cmp_ok($m->{'merge violations'}, '<', 512 * 1024,
		"$n violations: merged again, the merge holds less than 512kB of them");
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-publishing-violations');
		SELECT injection_points_wakeup('prolly-merge-publishing-violations')});
	$s->query_safe('SELECT 1');
	# (the new rows collide with ours' rows recorded already)
	is($s->query_safe(q{SELECT count(*) FROM dolt_constraint_violations('s.t')}),
		3 * $n, "$n violations: those found again are kept once");
	is($s->query_safe(q{SELECT count(*) FROM (
		SELECT violating_row->>'pk' AS pk,
		  row_number() OVER () AS place
		FROM dolt_constraint_violations('s.t')) v
		WHERE (place <= } . (2 * $n) . q{) = ((pk::int) < 20000000)}),
		3 * $n, "$n violations: the new ones follow those the branch had");
	is($s->query_safe(q{SELECT num_violations FROM dolt_constraint_violations()}),
		3 * $n, "$n violations: the summary counts them all");
	is(temp_bytes($db, $pid), 0, "$n violations: the merges remove their files");
	$s->quit;
}
# (a sort's memory past work_mem is its merge's buffers, fixed by work_mem)
cmp_ok($first[1], '<=', $first[0] * 1.5,
	'ten times as many violations do not take ten times the memory');
cmp_ok($second[1], '<=', $second[0] * 1.5,
	'nor do ten times as many the branch has');
cmp_ok($adding[1], '<=', $adding[0] * 1.5,
	'nor does publishing ten times as many');
cmp_ok($statement[1], '<=', $statement[0] * 1.5,
	'nor does the merge statement, the branch having ten times as many');
cmp_ok($disk[1], '>', $disk[0], 'more violations go to temporary files');
note("violations held: @first and @second bytes, @adding publishing, statement @statement; on disk: @disk bytes");
$node->stop;
done_testing();
