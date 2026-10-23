# Copyright (c) 2026, PostgreSQL Global Development Group

# Constraint checks stream what they find: a merge's foreign key check
# gives each violating row to the merge's violations as the query returns
# it, holding no result whole, and dolt_verify_constraints() spools the
# keys of the rows changed and makes each batch's query only as it runs it.
# Memory doesn't grow with how many rows a check finds or matches.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}
my $node = PostgreSQL::Test::Cluster->new('constraint_streams');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
work_mem = 64kB
dolt.allow_commit_conflicts = on
});
$node->start;

# Bytes the statement's contexts hold: its portal's and its transaction's,
# with those under them, from the log pg_log_backend_memory_contexts()
# writes, but for the chunk store's caches ("prolly ..."), which hold the
# chunks the fold of the working set read and wrote, not the checks'.
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

# Wait at the first check, measure the statement's memory, and let the
# rest run.
sub at_first_check
{
	my ($db, $pid) = @_;
	$node->wait_for_event('client backend', 'prolly-merge-constraint-checked');
	my $bytes = statement_memory($db, $pid);
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-constraint-checked');
		SELECT injection_points_wakeup('prolly-merge-constraint-checked')});
	return $bytes;
}

# Theirs deletes the parent all of ours' new rows reference: the merge's
# check of the foreign key finds every one.
my @merge;
for my $n (2000, 20000)
{
	my $db = "fk$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.parent (id int PRIMARY KEY);
		CREATE TABLE s.child (id int PRIMARY KEY, p int REFERENCES s.parent, pad text);
		INSERT INTO s.parent VALUES (1), (2);
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		INSERT INTO s.child SELECT g, 1, repeat(md5(g::text), 8)
		  FROM generate_series(1, $n) g;
		INSERT INTO s.child VALUES (0, 2, 'kept');
		SELECT dolt_commit('-Am', 'ours');
		SELECT dolt_checkout('side');
		DELETE FROM s.parent WHERE id = 1;
		SELECT dolt_commit('-Am', 'theirs');
		SELECT dolt_checkout('main');
	});
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-constraint-checked', 'wait')});
	$s->query_until(qr/merging/, q{\echo merging
		SELECT dolt_merge('side');
	});
	push @merge, at_first_check($db, $pid);
	$s->query_safe('SELECT 1');
	is($s->query_safe(q{SELECT count(*), count(DISTINCT violating_row->>'id'),
		bool_and(violation_type = 'foreign key')
		FROM dolt_constraint_violations('s.child')}), "$n|$n|t",
		"$n children: each orphan is recorded once");
	is($s->query_safe(q{SELECT count(*) FROM dolt_constraint_violations('s.child')
		WHERE violating_row->>'id' = '0'}), 0,
		"$n children: a child whose parent is there is not");
	$s->quit;
}
cmp_ok($merge[1], '<', $merge[0] + 64 * 1024,
	'ten times as many violations found take no more memory');
note("statement memory at the merge's check: @merge bytes");

# Rows changed since the head, checked by dolt_verify_constraints() a batch
# of keys at a time: a few of them orphans.
my @verify;
for my $n (5000, 50000)
{
	my $db = "verify$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.parent (id int PRIMARY KEY);
		CREATE TABLE s.child (id int PRIMARY KEY, p int REFERENCES s.parent, pad text);
		INSERT INTO s.parent VALUES (1);
		SELECT dolt_commit('-Am', 'base');
		INSERT INTO s.child SELECT g, 1, repeat(md5(g::text), 4)
		  FROM generate_series(1, $n) g;
		SET session_replication_role = replica;
		INSERT INTO s.child SELECT -g, 9, 'orphan' FROM generate_series(1, 5) g;
	});
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-constraint-checked', 'wait')});
	$s->query_until(qr/verifying/, q{\echo verifying
		SELECT dolt_verify_constraints('s.child');
	});
	push @verify, at_first_check($db, $pid);
	$s->query_safe('SELECT 1');
	is($s->query_safe(q{SELECT string_agg(violating_row->>'id', ' ' ORDER BY (violating_row->>'id')::int)
		FROM dolt_constraint_violations('s.child')}), '-5 -4 -3 -2 -1',
		"$n rows changed: the orphans among them are found");
	is($node->safe_psql($db, qq{SELECT count(*) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'}), 0, "$n rows changed: the keys' files are removed");
	$s->quit;
}
cmp_ok($verify[1], '<', $verify[0] + 64 * 1024,
	'ten times as many rows changed take no more memory');
note("statement memory at the first batch: @verify bytes");
$node->stop;
done_testing();
