# Copyright (c) 2026, PostgreSQL Global Development Group

# PostGIS's own regression suite (regress/run_test.pl, with its own hooks)
# on versioned tables, judged against heap from the same build.  Each suite
# runs once per mode, in a database run_test.pl makes:
#
#	heap           public as stock Postgres has it (the baseline)
#	prolly         public versioned, so PostGIS and the tables tests make
#	               in public are in a versioned schema; each table made in
#	               public is checked to be a prolly table (or an
#	               extension's own) as it is made
#	prolly_txn     the same, every transaction ending in a Dolt commit
#	               (dolt.transaction_commit), so what each test writes is
#	               folded as it goes
#	heap_dump, prolly_dump  run_test.pl --dumprestore: once PostGIS is
#	               installed, before any test, the database is dumped and
#	               restored (so pg_dump and restore of a versioned public
#	               holding PostGIS's extensions), and the tests run in the
#	               restored database
#
# Schemas a suite makes itself (topology's, from CreateTopology, and a few
# tests' own) are ordinary schemas, so their tables are heap tables beside
# the versioned public: topology runs beside prolly more than on it.  The
# summary reports, per suite and mode, how many of the tables the tests
# made were prolly tables, and how many were made elsewhere.
#
# A prolly mode's tests are compared with its baseline's (heap for prolly
# and prolly_txn, heap_dump for prolly_dump), by outcome and, for a test
# that failed in both, by what it printed (run_test.pl's obtained output,
# the run's database name aside):
#	agree               the same outcome, and the same output if both failed
#	regression          anything else
#	postgres_semantics  an intended difference, listed in
#	                    postgis_regress_known with its issue and its diff
#	known_bug           a difference listed there with the issue that will
#	                    fix it and its diff
#	crashed, timeout    the server crashed, or the run didn't finish
# A listed test whose diff isn't the listed one is a regression, and so is
# a listed test that agrees: the list stays exactly the known differences.
# The group fails on a regression, a crash or a timeout.  The
# classification is written to tmp_check/postgis_regress/summary.txt.
#
# POSTGIS_BUILD is a build of the pinned PostGIS (the Makefile's
# check-postgis-regress installs it into the temporary installation);
# POSTGIS_REGRESS_SUITES (core raster topology, and sfcgal when built)
# and POSTGIS_REGRESS_MODES (all five) choose what runs.

use strict;
use warnings FATAL => 'all';
use Cwd qw(abs_path);
use Digest::MD5 qw(md5_hex);
use File::Path qw(make_path);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $build = $ENV{POSTGIS_BUILD}
  or BAIL_OUT('POSTGIS_BUILD is not set (see src/test/prolly/README)');
my $src = "$build/src";
-f "$src/regress/run_test.pl" or BAIL_OUT("$build is not a PostGIS build directory");

my %suite_dir = (
	core => 'regress',
	raster => 'raster/test/regress',
	topology => 'topology/test',
	sfcgal => 'sfcgal/regress');
my $default_suites = 'core raster topology'
  . (-f "$src/sfcgal/postgis_sfcgal-3.so" ? ' sfcgal' : '');
my @suites = split ' ', ($ENV{POSTGIS_REGRESS_SUITES} // $default_suites);
my @modes = split ' ',
  ($ENV{POSTGIS_REGRESS_MODES} // 'heap prolly prolly_txn heap_dump prolly_dump');
my %baseline = (prolly => 'heap', prolly_txn => 'heap', prolly_dump => 'heap_dump');
my $timeout = $ENV{POSTGIS_REGRESS_TIMEOUT} // 3600;

for my $s (@suites)
{
	BAIL_OUT("unknown PostGIS suite $s") unless $suite_dir{$s};
}
for my $m (@modes)
{
	BAIL_OUT("unknown mode $m")
	  unless $m =~ /\A(?:heap|prolly|prolly_txn|heap_dump|prolly_dump)\z/;
	BAIL_OUT("mode $m needs its baseline $baseline{$m} too")
	  if $baseline{$m} && !grep { $_ eq $baseline{$m} } @modes;
}

my $out = "$PostgreSQL::Test::Utils::tmp_check/postgis_regress";
make_path($out);
my $here = abs_path('.');

# Known differences: "TEST MODE ISSUE KIND DIFF REASON...", DIFF the md5 of
# the test's diff on that mode (diff_md5 below).
my %known;
for my $line (split /\n/, slurp_file("$here/postgis_regress_known"))
{
	next if $line =~ /^\s*(#|$)/;
	my ($test, $mode, $issue, $kind, $diff) = split ' ', $line;
	BAIL_OUT("postgis_regress_known: $line")
	  unless $kind && $kind =~ /\A(?:intended|bug)\z/ && $issue =~ /\A#\d+\z/
	  && $diff && $diff =~ /\A[0-9a-f]{32}\z/;
	$known{"$mode $test"} = {
		issue => $issue, diff => $diff,
		class => $kind eq 'intended' ? 'postgres_semantics' : 'known_bug' };
}

my $node = PostgreSQL::Test::Cluster->new('postgis_regress');
$node->init;
$node->append_conf('postgresql.conf', qq(
max_prepared_transactions = 10
lc_messages = 'C'
));
$node->start;

# The scripts the prolly modes add to run_test.pl's.
my $versioned = "$out/versioned_public.sql";
my $txn_commit = "$out/txn_commit.sql";
my $watch = "$out/watch_tables.sql";
PostgreSQL::Test::Utils::append_to_file($versioned, q{
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
GRANT ALL ON SCHEMA public TO PUBLIC;
});
# (each transaction's commit moves main's head, which takes the database's
# CREATE among the privileges that may, as a deployment turning on
# transaction commits would grant: the tests' own roles make tables too)
PostgreSQL::Test::Utils::append_to_file($txn_commit, q{
DO $$ BEGIN
	EXECUTE format('ALTER DATABASE %I SET dolt.transaction_commit = on', current_database());
	EXECUTE format('GRANT CREATE ON DATABASE %I TO PUBLIC', current_database());
END $$;
});
# Every table a test makes is recorded as it is made, by an event trigger
# in an ordinary schema (tests drop their tables: looking after the test
# would find none).  It runs as its owner, on pg_catalog's path, as tests
# make tables as other roles and some put objects named as catalogs on the
# search path.
PostgreSQL::Test::Utils::append_to_file($watch, q{
CREATE SCHEMA pgis_prolly_check;
CREATE TABLE pgis_prolly_check.made (nsp name, rel text, am name);
CREATE SEQUENCE pgis_prolly_check.tests;
CREATE FUNCTION pgis_prolly_check.made() RETURNS event_trigger LANGUAGE plpgsql
	SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE obj record;
BEGIN
	FOR obj IN SELECT * FROM pg_event_trigger_ddl_commands()
		WHERE NOT in_extension AND object_type IN ('table', 'materialized view')
	LOOP
		INSERT INTO pgis_prolly_check.made
		SELECT c.relnamespace::regnamespace::name, obj.object_identity, a.amname
		FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.oid = obj.objid AND c.relnamespace <> 'pgis_prolly_check'::regnamespace;
	END LOOP;
END $$;
CREATE EVENT TRIGGER pgis_prolly_check ON ddl_command_end
	EXECUTE FUNCTION pgis_prolly_check.made();
});

# After each test, in a prolly mode: what it made is written to the run's
# coverage directory, and a heap table in public fails the run.
sub after_test_script
{
	my ($dir) = @_;
	my $path = "$out/after_test_" . ($dir =~ s{.*/}{}r) . ".sql";
	PostgreSQL::Test::Utils::append_to_file($path, qq{
DO \$\$
DECLARE wrong text;
BEGIN
	EXECUTE format('COPY pgis_prolly_check.made TO %L',
		'$dir/' || nextval('pgis_prolly_check.tests'));
	SELECT string_agg(rel || ' on ' || am, ', ') INTO wrong FROM pgis_prolly_check.made
	WHERE nsp = 'public' AND am <> 'prolly';
	DELETE FROM pgis_prolly_check.made;
	IF wrong IS NOT NULL THEN
		RAISE EXCEPTION 'tables in public not on prolly: %', wrong;
	END IF;
END \$\$;
});
	return $path;
}

# run_test.pl's command for a suite, as its check-regress target runs it.
sub suite_command
{
	my ($suite, $dir) = @_;
	# (topology's tests load files its Makefile generates)
	if ($suite eq 'topology')
	{
		system("make -s -C '$dir' check-regress-deps >/dev/null 2>&1") == 0
		  or BAIL_OUT("make check-regress-deps failed in $dir");
	}
	my $plan = `make -s -n -C '$dir' check-regress RUNTESTFLAGS=--extensions 2>/dev/null`;
	my ($cmd) = grep { /run_test\.pl/ } split /\n/, $plan;
	$cmd or BAIL_OUT("no run_test.pl command in ${dir}'s check-regress");
	return $cmd;
}

# Each test's outcome in a run_test.pl log: ok, failed or skipped, with its
# obtained output and diff files when it failed on its output.  A failure
# printed after a test's ok (object counts, cleanup, uninstall) fails it.
sub outcomes
{
	my ($log, $tmp) = @_;
	my (%o, $last);
	for my $line (split /\n/, $log)
	{
		if ($line =~ /^\s(\S.*?) \.+ (ok|failed|skipped)\b(.*)/)
		{
			$last = $1;
			$o{$last} = { status => $2, reason => $3 };
			$o{$last}{status} = 'failed' if $3 =~ /\bfailed \(/;
		}
		elsif ($last && $line =~ /\bfailed \((.*)\)/)
		{
			$o{$last}{status} = 'failed';
			$o{$last}{reason} .= " failed ($1)";
		}
		else
		{
			next;
		}
		my ($n) = ($o{$last}{reason} =~ m{/test_(\d+)_diff\b});
		if (defined $n)
		{
			$o{$last}{output} = "$tmp/test_${n}_out";
			$o{$last}{diff} = "$tmp/test_${n}_diff";
		}
	}
	return \%o;
}

# What a failed test printed, the run's database name made generic.
sub output_of
{
	my ($t, $db) = @_;
	my $text = $t->{output} && -f $t->{output} ? slurp_file($t->{output}) : $t->{reason} // '';
	return $text =~ s/\Q$db\E/DB/gr;
}

# A test's diff (expected against obtained), its file names, times and the
# database name aside, as an md5.
sub diff_md5
{
	my ($t, $db) = @_;
	return 'none' unless $t->{diff} && -f $t->{diff};
	my $diff = join("\n", grep { !/^(?:---|\+\+\+) / } split /\n/, slurp_file($t->{diff}));
	return md5_hex($diff =~ s/\Q$db\E/DB/gr);
}

# The tables a run's tests made, from its coverage directory: the number
# on prolly in public, and the others by schema and access method.
sub coverage
{
	my ($dir) = @_;
	my ($prolly, %elsewhere) = (0);
	opendir(my $dh, $dir) or return (0, {});
	for my $f (grep { /^\d+$/ } readdir $dh)
	{
		for my $row (split /\n/, slurp_file("$dir/$f"))
		{
			my ($nsp, $rel, $am) = split /\t/, $row;
			# (temporary tables, which can't be versioned, counted as one)
			$nsp = 'temporary schemas' if $nsp =~ /^pg_temp_\d+$/;
			if ($nsp eq 'public' && $am eq 'prolly') { $prolly++ }
			else { $elsewhere{"$nsp on $am"}++ }
		}
	}
	closedir $dh;
	return ($prolly, \%elsewhere);
}

sub quote
{
	my ($s) = @_;
	$s =~ s/'/'\\''/g;
	return "'$s'";
}

my %results;	# mode => suite => {outcomes, status, db, coverage}
for my $suite (@suites)
{
	my $dir = "$src/$suite_dir{$suite}";
	my $base = suite_command($suite, $dir);
	for my $mode (@modes)
	{
		my $flags = '--extensions';
		$flags .= " --dumprestore" if $mode =~ /_dump\z/;
		my $covdir = "$out/coverage_${suite}_$mode";
		if ($mode =~ /\Aprolly/)
		{
			make_path($covdir);
			chmod 0777, $covdir;
			$flags .= " --after-create-db-script $versioned --after-create-db-script $watch";
			$flags .= " --after-create-db-script $txn_commit" if $mode eq 'prolly_txn';
			$flags .= " --after-test-script " . after_test_script($covdir);
		}
		(my $cmd = $base) =~ s/--extensions/$flags/;
		my $tmp = "$out/tmp_${suite}_$mode";
		make_path($tmp);
		my $log = "$out/${suite}_$mode.log";
		my $db = "postgis_reg_${suite}_$mode";
		local $ENV{PGHOST} = $node->host;
		local $ENV{PGPORT} = $node->port;
		local $ENV{PGUSER} = $ENV{USER} // getpwuid($<);
		local $ENV{POSTGIS_REGRESS_DB} = $db;
		local $ENV{PGIS_REG_TMPDIR} = $tmp;
		my $offset = -s $node->logfile;
		my $rc = system("cd '$dir' && timeout $timeout sh -c " . quote($cmd) . " > '$log' 2>&1");
		my $text = slurp_file($log);
		my $server = slurp_file($node->logfile, $offset);
		my $status =
		    ($rc >> 8) == 124 ? 'timeout'
		  : $server =~ /was terminated by signal|server process .* exited with exit code [^0]/ ? 'crashed'
		  : ($text . (-f "$tmp/regress_log.tmp" ? slurp_file("$tmp/regress_log.tmp") : ''))
			=~ /tables in public not on prolly: (.*)/ ? "not on prolly: $1"
		  : $text =~ /^Run tests: \d+/m ? 'finished'
		  : 'aborted';
		my ($prolly, $elsewhere) = $mode =~ /\Aprolly/ ? coverage($covdir) : (0, {});
		$results{$mode}{$suite} = { outcomes => outcomes($text, $tmp), status => $status,
			db => $db, prolly => $prolly, elsewhere => $elsewhere };
		# (a crash leaves the server restarting; give it a clean start)
		$node->restart if $status eq 'crashed';
		my $o = $results{$mode}{$suite}{outcomes};
		diag(sprintf('%-9s %-11s %s, %d tests, %d failed', $suite, $mode, $status,
			scalar(keys %$o), scalar(grep { $_->{status} eq 'failed' } values %$o)));
	}
}

# Classify each prolly mode's tests against its baseline.
my (@summary, @coverage, %count);
for my $mode (grep { $baseline{$_} } @modes)
{
	for my $suite (@suites)
	{
		my $r = $results{$mode}{$suite};
		my $b = $results{ $baseline{$mode} }{$suite};
		ok($b->{status} eq 'finished', "$suite on $baseline{$mode} finished")
		  or diag("$suite $baseline{$mode}: $b->{status}");
		my %elsewhere = %{ $r->{elsewhere} };
		my $others = 0;
		$others += $_ for values %elsewhere;
		push @coverage, sprintf("%s %s: %d tables made on prolly in public, %d elsewhere%s",
			$mode, $suite, $r->{prolly}, $others,
			$others ? ' (' . join(', ', map { "$_: $elsewhere{$_}" } sort keys %elsewhere) . ')' : '');
		if ($r->{status} ne 'finished')
		{
			my $class = $r->{status} =~ /\A(?:crashed|timeout)\z/ ? $r->{status} : 'regression';
			$count{$class}++;
			push @summary, "$mode $suite (whole suite) $class: $r->{status}";
			fail("$suite on $mode finished ($r->{status})");
			next;
		}
		my %tests = (%{ $b->{outcomes} }, %{ $r->{outcomes} });
		my @bad;
		for my $test (sort keys %tests)
		{
			my $have = $r->{outcomes}{$test} // { status => 'missing' };
			my $want = $b->{outcomes}{$test} // { status => 'missing' };
			my $same = $have->{status} eq $want->{status}
			  && ($have->{status} ne 'failed'
				|| output_of($have, $r->{db}) eq output_of($want, $b->{db}));
			my $known = $known{"$mode $test"};
			my $diff = diff_md5($have, $r->{db});
			my $class =
			    $same ? ($known ? 'listed_but_agrees' : 'agree')
			  : $known && $known->{diff} eq $diff ? $known->{class}
			  : 'regression';
			$count{$class}++;
			next if $class eq 'agree';
			my $line = "$mode $test $class: $have->{status} (baseline $want->{status}), diff $diff"
			  . ($known ? " (listed $known->{issue}, diff $known->{diff})" : '');
			push @summary, $line;
			push @bad, $line if $class =~ /\A(?:regression|listed_but_agrees)\z/;
		}
		is(join("\n", @bad), '', "$suite on $mode agrees with $baseline{$mode}");
	}
}
my $text = join("\n", map({ "$_: $count{$_}" } sort keys %count), '', @coverage, '', @summary) . "\n";
PostgreSQL::Test::Utils::append_to_file("$out/summary.txt", $text);
diag($text);
$node->stop;
done_testing();
