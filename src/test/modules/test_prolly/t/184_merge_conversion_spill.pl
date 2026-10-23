# Copyright (c) 2026, PostgreSQL Global Development Group

# A merge across a type change of a table's key reads each version's rows
# as the table is defined now, in another key order: the rows are sorted by
# a tuplesort that spills past work_mem, and the base's rows with unsure
# values are spooled to a file in key order, rather than held in arrays.
# Memory doesn't grow with the table; the merged rows are as before.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}
my $node = PostgreSQL::Test::Cluster->new('conversion_spill');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
work_mem = 64kB
});
$node->start;

# Bytes the statement's contexts hold: its portal's and its transaction's,
# with those under them, from the log pg_log_backend_memory_contexts()
# writes, but for the chunk store's caches ("prolly ...").
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

# Both sides widen the key and narrow v (each having shortened every v
# first): the base's v, too long for varchar(3), is unsure in every row.
# Theirs changes half the rows' pad too.
my (@memory, @disk);
for my $n (10000, 100000)
{
	my $db = "conv$n";
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.t (id int PRIMARY KEY, v text, pad text);
		INSERT INTO s.t SELECT g, repeat('x', 10), repeat(md5(g::text), 4)
		  FROM generate_series(1, $n) g;
		SELECT dolt_commit('-Am', 'base');
		SELECT dolt_branch('side');
		UPDATE s.t SET v = 'ok';
		ALTER TABLE s.t ALTER COLUMN id TYPE bigint, ALTER COLUMN v TYPE varchar(3);
		SELECT dolt_commit('-Am', 'ours');
		SELECT dolt_checkout('side');
		UPDATE s.t SET v = 'ok';
		ALTER TABLE s.t ALTER COLUMN id TYPE bigint, ALTER COLUMN v TYPE varchar(3);
		UPDATE s.t SET pad = 'theirs' WHERE id % 2 = 0;
		SELECT dolt_commit('-Am', 'theirs');
		SELECT dolt_checkout('main');
	});
	my $s = $node->background_psql($db, on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-merge-rows-converted', 'wait')});
	$s->query_until(qr/merging/, q{\echo merging
		SELECT dolt_merge('side');
	});
	$node->wait_for_event('client backend', 'prolly-merge-rows-converted');
	push @memory, statement_memory($db, $pid);
	push @disk, $node->safe_psql($db, qq{SELECT coalesce(sum(size), 0)
		FROM pg_ls_tmpdir() WHERE name LIKE 'pgsql_tmp$pid.%'});
	$node->safe_psql($db, q{SELECT injection_points_detach('prolly-merge-rows-converted');
		SELECT injection_points_wakeup('prolly-merge-rows-converted')});
	$s->query_safe('SELECT 1');
	is($s->query_safe(q{SELECT count(*), count(*) FILTER (WHERE v = 'ok'),
		count(*) FILTER (WHERE pad = 'theirs') FROM s.t}),
		"$n|$n|" . int($n / 2), "$n rows: merged as both sides changed them");
	is($s->query_safe(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
		WHERE attrelid = 's.t'::regclass AND attnum > 0 ORDER BY attnum}),
		"bigint\ncharacter varying(3)\ntext", "$n rows: as defined now");
	is($s->query_safe(q{SELECT test_fold_root(true) = prolly_root_hash('HEAD')}), 't',
		"$n rows: the merge commit's rows match a full fold");
	is($node->safe_psql($db, qq{SELECT count(*) FROM pg_ls_tmpdir()
		WHERE name LIKE 'pgsql_tmp$pid.%'}), 0, "$n rows: the merge removes its files");
	$s->quit;
}
cmp_ok($memory[1], '<', $memory[0] + 128 * 1024,
	'ten times as many rows converted take no more memory');
cmp_ok($disk[1], '>', $disk[0] * 5, 'the rows converted go to temporary files');
note("statement memory while converting: @memory bytes; temporary files: @disk bytes");
# Rows that come together under the definition now, sorted past work_mem:
# shown (dolt_at()), a keyed table's rows sharing a key are each shown once
# and the rest counted, and a keyless table's rows made alike are one row
# with their copies counted; the same rows however the sort spilled.
{
	my $n = 50000;
	my $db = 'dups';
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	$node->safe_psql($db, qq{
		CREATE EXTENSION test_prolly;
		CREATE SCHEMA s VERSIONED;
		CREATE TABLE s.kl (i int, j int, pad text);
		INSERT INTO s.kl SELECT g, j, repeat(md5((g * j)::text), 2)
		  FROM generate_series(1, $n) g, generate_series(1, 2) j;
		CREATE TABLE s.kd (i int, j int);
		INSERT INTO s.kd SELECT g, j FROM generate_series(1, $n) g, generate_series(1, 2) j;
		SELECT dolt_commit('-Am', 'keyless');
		DELETE FROM s.kl WHERE j = 2;
		ALTER TABLE s.kl ADD PRIMARY KEY (i);
		ALTER TABLE s.kd DROP COLUMN j;
		SELECT dolt_commit('-Am', 'kl keyed, kd without j');
	});
	my $shown = q{SELECT count(*), md5(string_agg(format('%s:%s:%s', i, j, pad), ' ' ORDER BY i))
		FROM dolt_at('HEAD~1', NULL::s.kl)};
	my ($ret, $spilled, $err) = $node->psql($db, $shown);
	like($err, qr/$n rows of a version of table "kl" share a primary key with others as the key is defined now: each key shown once/,
		'shown, the rows sharing a key are counted');
	($ret, my $in_memory) = $node->psql($db, "SET work_mem = '256MB'; $shown");
	like($spilled, qr/^$n\|/, 'each key is shown once');
	is($spilled, $in_memory, 'the rows shown are those an unspilled sort shows');
	# (a keyless row's key leads with its values: of a key's rows, j = 1's
	# is read first)
	is($node->safe_psql($db, q{SELECT count(*) FILTER (WHERE j = 1)
		FROM dolt_at('HEAD~1', NULL::s.kl)}), $n, "each key's first row read is the one shown");
	my $copies = q{SELECT count(*), count(DISTINCT i) FROM dolt_at('HEAD~1', NULL::s.kd)};
	is($node->safe_psql($db, $copies), 2 * $n . "|$n",
		'a keyless row made alike is counted with its copies');
	is($node->safe_psql($db, "SET work_mem = '256MB'; $copies"), 2 * $n . "|$n",
		'as an unspilled sort counts them');
}
$node->stop;
done_testing();
