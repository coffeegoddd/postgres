# Copyright (c) 2026, PostgreSQL Global Development Group

# PostGIS's own regression suite (regress/run_test.pl, with its own hooks)
# on versioned tables, judged against heap from the same build.  Each suite
# runs once per mode, in a database run_test.pl makes:
#
#	heap           public as stock Postgres has it (the baseline)
#	prolly         public versioned, so PostGIS and every table a test
#	               makes are in a versioned schema; after each test, every
#	               table in public must be a prolly table (or an
#	               extension's own)
#	prolly_txn     the same, every transaction ending in a Dolt commit
#	               (dolt.transaction_commit), so what each test writes is
#	               folded as it goes
#	heap_dump, prolly_dump  run_test.pl --dumprestore: each test's
#	               database dumped and restored, so pg_dump of a versioned
#	               public holding PostGIS columns is exercised
#
# A prolly mode's outcome for a test is classified against its heap
# baseline (heap for prolly and prolly_txn, heap_dump for prolly_dump):
# agree (the same outcome), regression (heap passes, prolly doesn't),
# postgres_semantics (an intended difference, listed in
# postgis_regress_known with its issue), known_bug (a difference listed
# there with the issue that will fix it), crashed (the server crashed
# during the run) or timeout (the run didn't finish in time).  The test
# fails on any regression, crash or timeout, or on a listed difference
# that no longer differs.  The classification is written to
# tmp_check/postgis_regress/summary.txt.
#
# POSTGIS_BUILD is a build of the pinned PostGIS (the Makefile's
# check-postgis-regress installs it into the temporary installation);
# POSTGIS_REGRESS_SUITES (core raster topology, and sfcgal when built)
# and POSTGIS_REGRESS_MODES (all five) choose what runs.

use strict;
use warnings FATAL => 'all';
use Cwd qw(abs_path);
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

# Intended differences: "suite/test mode issue reason", one per line.
my %known;
for my $line (split /\n/, slurp_file("$here/postgis_regress_known"))
{
	next if $line =~ /^\s*(#|$)/;
	my ($test, $mode, $issue, $kind) = split ' ', $line;
	BAIL_OUT("postgis_regress_known: $line")
	  unless $kind && $kind =~ /\A(?:intended|bug)\z/ && $issue =~ /\A#\d+\z/;
	$known{"$mode $test"} = [ $issue, $kind eq 'intended' ? 'postgres_semantics' : 'known_bug' ];
}

my $node = PostgreSQL::Test::Cluster->new('postgis_regress');
$node->init;
$node->append_conf('postgresql.conf', qq(
max_prepared_transactions = 10
lc_messages = 'C'
));
$node->start;

# The scripts each mode adds to run_test.pl's.
my %scripts = (
	versioned => "$out/versioned_public.sql",
	txn_commit => "$out/txn_commit.sql",
	watch => "$out/watch_tables.sql",
	on_prolly => "$out/on_prolly.sql");
PostgreSQL::Test::Utils::append_to_file($scripts{versioned}, q{
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
GRANT ALL ON SCHEMA public TO PUBLIC;
});
PostgreSQL::Test::Utils::append_to_file($scripts{txn_commit}, q{
DO $$ BEGIN
	EXECUTE format('ALTER DATABASE %I SET dolt.transaction_commit = on', current_database());
END $$;
});
# Every table a test makes in public must be a prolly table (or an
# extension's own): a heap table there would let the test pass on heap.
# Tests drop their tables, so tables are checked as they are made, by an
# event trigger in an ordinary schema, and after each test the ones that
# weren't fail the run.  (Tests make tables as other roles, and some put
# objects named as catalogs on the search path: the trigger runs as its
# owner, on pg_catalog's path.)
PostgreSQL::Test::Utils::append_to_file($scripts{watch}, q{
CREATE SCHEMA pgis_prolly_check;
CREATE TABLE pgis_prolly_check.not_prolly (rel text);
CREATE FUNCTION pgis_prolly_check.made() RETURNS event_trigger LANGUAGE plpgsql
	SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $$
DECLARE obj record;
BEGIN
	FOR obj IN SELECT * FROM pg_event_trigger_ddl_commands()
		WHERE NOT in_extension AND object_type IN ('table', 'materialized view')
	LOOP
		INSERT INTO pgis_prolly_check.not_prolly
		SELECT obj.object_identity || ' on ' || a.amname
		FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.oid = obj.objid AND c.relnamespace = 'public'::regnamespace
		AND a.amname <> 'prolly';
	END LOOP;
END $$;
CREATE EVENT TRIGGER pgis_prolly_check ON ddl_command_end
	EXECUTE FUNCTION pgis_prolly_check.made();
});
PostgreSQL::Test::Utils::append_to_file($scripts{on_prolly}, q{
DO $$
DECLARE wrong text;
BEGIN
	SELECT string_agg(rel, ', ') INTO wrong FROM pgis_prolly_check.not_prolly;
	IF wrong IS NOT NULL THEN
		RAISE EXCEPTION 'tables in public not on prolly: %', wrong;
	END IF;
END $$;
});

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

# Each test's outcome in a run_test.pl log: ok, failed or skipped.
sub outcomes
{
	my ($log) = @_;
	my %o;
	for my $line (split /\n/, $log)
	{
		next unless $line =~ /^\s(\S+) \.+ (ok|failed|skipped)\b/;
		$o{$1} = $2;
	}
	return \%o;
}

my %results;	# mode => suite => {outcomes, status}
for my $suite (@suites)
{
	my $dir = "$src/$suite_dir{$suite}";
	my $base = suite_command($suite, $dir);
	for my $mode (@modes)
	{
		my $flags = '--extensions';
		$flags .= " --dumprestore" if $mode =~ /_dump\z/;
		if ($mode =~ /\Aprolly/)
		{
			$flags .= " --after-create-db-script $scripts{versioned}";
			$flags .= " --after-create-db-script $scripts{watch}";
			$flags .= " --after-create-db-script $scripts{txn_commit}" if $mode eq 'prolly_txn';
			$flags .= " --after-test-script $scripts{on_prolly}";
		}
		(my $cmd = $base) =~ s/--extensions/$flags/;
		my $tmp = "$out/tmp_${suite}_$mode";
		make_path($tmp);
		my $log = "$out/${suite}_$mode.log";
		local $ENV{PGHOST} = $node->host;
		local $ENV{PGPORT} = $node->port;
		local $ENV{PGUSER} = $ENV{USER} // getpwuid($<);
		local $ENV{POSTGIS_REGRESS_DB} = "postgis_reg_${suite}_$mode";
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
		$results{$mode}{$suite} = { outcomes => outcomes($text), status => $status };
		# (a crash leaves the server restarting; give it a clean start)
		$node->restart if $status eq 'crashed';
		diag(sprintf('%-9s %-11s %s, %d tests, %d failed', $suite, $mode, $status,
			scalar(keys %{ $results{$mode}{$suite}{outcomes} }),
			scalar(grep { $_ eq 'failed' } values %{ $results{$mode}{$suite}{outcomes} })));
	}
}

sub quote
{
	my ($s) = @_;
	$s =~ s/'/'\\''/g;
	return "'$s'";
}

# Classify each prolly mode's tests against its baseline.
my @summary;
my %count;
for my $mode (grep { $baseline{$_} } @modes)
{
	for my $suite (@suites)
	{
		my $r = $results{$mode}{$suite};
		my $b = $results{ $baseline{$mode} }{$suite};
		ok($b->{status} eq 'finished', "$suite on $baseline{$mode} finished")
		  or diag("$suite $baseline{$mode}: $b->{status}");
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
			my $have = $r->{outcomes}{$test} // 'missing';
			my $want = $b->{outcomes}{$test} // 'missing';
			my ($issue, $listed) = @{ $known{"$mode $test"} // [] };
			my $class =
			    $have eq $want ? ($issue ? 'listed_but_agrees' : 'agree')
			  : $issue ? $listed
			  : $want eq 'ok' ? 'regression'
			  : 'agree';	# (heap failed too, differently: not prolly's)
			$count{$class}++;
			next if $class eq 'agree' && $have eq $want;
			my $line = "$mode $test $class: $have (baseline $want)" . ($issue ? " $issue" : '');
			push @summary, $line;
			push @bad, $line if $class =~ /\A(?:regression|listed_but_agrees)\z/;
		}
		is(join("\n", @bad), '', "$suite on $mode agrees with $baseline{$mode}");
	}
}
my $text = join("\n",
	map({ "$_: $count{$_}" } sort keys %count), '', @summary) . "\n";
PostgreSQL::Test::Utils::append_to_file("$out/summary.txt", $text);
diag($text);
$node->stop;
done_testing();
