# Copyright (c) 2026, PostgreSQL Global Development Group

# Alternative type interpretations retain only current outcomes in memory.
# Their scratch files grow with the work, and go away on success or cancel.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}
my $node = PostgreSQL::Test::Cluster->new('merge_plans');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
});
$node->start;
my (@memory, @disk);
for my $n (500, 5000)
{
	my $db = "plan$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.t (c char(5), id int, payload text);
		INSERT INTO s.t SELECT 'ab', g, repeat(md5(g::text), 16)
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		ALTER TABLE s.t ALTER COLUMN c TYPE text;
		INSERT INTO s.t VALUES ('ours', -1, 'ours');
		SELECT dolt_commit('-Am', 'ours');
		SELECT dolt_checkout('side');
		ALTER TABLE s.t ALTER COLUMN c TYPE text;
		INSERT INTO s.t VALUES ('theirs', -2, 'theirs');
		SELECT dolt_commit('-Am', 'theirs');
		SELECT dolt_checkout('main');
	});
	my $head = $node->safe_psql($db, "SELECT dolt_hashof('HEAD')");
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-plans-compared', 'wait')});
	$s->query_until(qr/planning/, q{\echo planning
		SELECT dolt_merge('side');
	});
	$node->wait_for_event('client backend', 'prolly-merge-plans-compared');
	my $offset = -s $node->logfile;
	$node->safe_psql($db, "SELECT pg_log_backend_memory_contexts($pid)");
	$node->wait_for_log(qr/merge outcome streams: \d+ total/, $offset);
	my $log = substr(slurp_file($node->logfile), $offset);
	my ($bytes) = $log =~ /merge outcome streams: (\d+) total/;
	push @memory, $bytes;
	cmp_ok($bytes, '<', 128 * 1024, "$n rows: outcome buffers stay below 128kB");
	my $files = $node->safe_psql($db, qq{SELECT count(*) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'});
	is($files, 2, "$n rows: the two outcome streams are on disk");
	push @disk, $node->safe_psql($db, qq{SELECT sum(size) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'});
	is($node->safe_psql($db, "SELECT dolt_hashof('HEAD')"), $head,
		"$n rows: validation finishes before publishing writes");
	$node->safe_psql($db, q{SELECT injection_points_wakeup('prolly-merge-plans-compared')});
	$s->query_safe('SELECT 1');
	is($s->query_safe("SELECT count(*) FROM s.t"), $n + 2,
		"$n rows: both insertions and every original row survive");
	is($s->query_safe(q{SELECT test_fold_root(true) = prolly_root_hash('HEAD')}), 't',
		"$n rows: result matches full folding");
	is($node->safe_psql($db, qq{SELECT count(*) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'}), 0, "$n rows: success removes scratch files");

	# Repeat from the unmerged head and cancel with the streams still open.
	$s->query_safe("SELECT dolt_reset('--hard', '$head')");
	$s->query_until(qr/canceling/, q{\echo canceling
		SELECT dolt_merge('side');
	});
	$node->wait_for_event('client backend', 'prolly-merge-plans-compared');
	$node->safe_psql($db, "SELECT pg_cancel_backend($pid)");
	$s->query('SELECT 1');
	like($s->{stderr}, qr/canceling statement due to user request/,
		"$n rows: the merge was canceled");
	$s->{stderr} = '';
	is($s->query_safe("SELECT dolt_hashof('HEAD')"), $head,
		"$n rows: cancellation leaves the head unchanged");
	is($s->query_safe('SELECT count(*) FROM s.t'), $n + 1,
		"$n rows: cancellation leaves the working rows unchanged");
	is($node->safe_psql($db, qq{SELECT count(*) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'}), 0, "$n rows: cancellation removes scratch files");
	$s->query_safe(q{SELECT injection_points_detach('prolly-merge-plans-compared')});
	# A genuine interpretation disagreement must also discard both spools
	# when the caller rolls back only to a savepoint.
	$s->query_safe(qq{SELECT dolt_checkout('side');
		DELETE FROM s.t WHERE id = $n;
		SELECT dolt_commit('-Am', 'ambiguous deletion');
		SELECT dolt_checkout('main');
		BEGIN; SAVEPOINT before_merge;});
	$s->query(q{SELECT dolt_merge('side')});
	like($s->{stderr}, qr/can't be merged across an ambiguous type change/,
		"$n rows: interpretations that disagree are refused");
	$s->{stderr} = '';
	$s->query_safe('ROLLBACK TO SAVEPOINT before_merge');
	is($s->query_safe("SELECT dolt_hashof('HEAD')"), $head,
		"$n rows: refusal leaves the head unchanged");
	is($s->query_safe('SELECT count(*) FROM s.t'), $n + 1,
		"$n rows: refusal leaves the working rows unchanged");
	is($node->safe_psql($db, qq{SELECT count(*) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'}), 0, "$n rows: savepoint rollback removes scratch files");
	$s->query_safe('COMMIT');
	$s->quit;
}
cmp_ok($memory[1], '<=', $memory[0] + 16384,
	'ten times as many outcomes do not retain ten times the memory');
cmp_ok($disk[1], '>', $disk[0] * 5, 'additional outcomes go to temporary storage');
note("outcome memory: @memory bytes; temporary storage: @disk bytes");
$node->stop;
done_testing();
