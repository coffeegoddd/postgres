# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset or a first checkout, at wal_level minimal, syncs no page
# store: its shares and snapshots log images of their own blocks, and skip
# their flush when they hold none.  They leave nothing recovery can't
# rebuild: a crash right after either, with a dirty working set elsewhere,
# then recovery, leaves every branch's tables as they were.  Then the same
# again with the pages written out and the cache cold, so the reads are of
# the page store.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('store_sync_skip_crash');
$node->init;
$node->append_conf('postgresql.conf',
	"autovacuum = off\ncheckpoint_timeout = 1h\nshared_buffers = 1MB");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	ALTER TABLE app.t ALTER pad SET STORAGE EXTERNAL;
	INSERT INTO app.t SELECT i, 0, CASE WHEN i % 100 = 0
		THEN repeat('y', 3000) ELSE repeat('x', 300) END
		FROM generate_series(1, 5000) i;
	CREATE TABLE app.u (id int PRIMARY KEY, w int);
	INSERT INTO app.u SELECT i, i FROM generate_series(1, 2000) i;
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('other');
	CHECKPOINT;
});

my $dirty = q{UPDATE app.t SET v = v + 1, pad = pad || 'd' WHERE id % 5 = 0;
	UPDATE app.u SET w = -w WHERE id % 3 = 0};
my $contents = q{SELECT count(*), sum(v), sum(length(pad)) FROM app.t;
	SELECT count(*), sum(w) FROM app.u;
	SELECT count(*) FROM dolt_status};

# What a branch's session reads, after checking it out ('main' is the
# session's own).
sub on_branch
{
	my ($branch) = @_;
	my $out = sql(($branch eq 'main' ? '' : "SELECT dolt_checkout('$branch');\n")
		  . $contents);
	$out =~ s/^\{0,"Switched to branch '[^']*'"\}\n//;
	return $out;
}

my $clean = on_branch('main');

# A crash, and recovery; then a hard reset of a dirty set.
sub crash
{
	$node->stop('immediate');
	$node->start;
}

sub warm_up
{
	sql($dirty);
	sql(q{SELECT dolt_reset('--hard')});
}

# Each case: a dirty working set, an operation, and an immediate stop right
# after it.
for my $cold (0, 1)
{
	my $how = $cold ? 'with the pages written out first' : 'with dirty buffers';

	# Hard reset: the dirty set discarded.
	sql($dirty);
	sql($dirty) if $cold;
	sql('CHECKPOINT') if $cold;
	sql(q{SELECT dolt_reset('--hard')});
	crash();
	is(on_branch('main'), $clean,
		"after a crash, the hard reset's tables are the commit's, $how");
	is(on_branch('other'), $clean, "and another branch's too, $how");
	warm_up();

	# First checkout of a fresh branch, the source's working set dirty.
	sql($dirty);
	my $dirtied = on_branch('main');
	isnt($dirtied, $clean, "main's working set is dirty, $how");
	sql('CHECKPOINT') if $cold;
	my $branch = $cold ? 'fresh_cold' : 'fresh';
	sql("SELECT dolt_branch('$branch')");
	sql(qq{SELECT dolt_checkout('$branch')});
	crash();
	is(on_branch($branch), $clean,
		"after a crash, the checked-out branch has the commit's rows, $how");
	is(on_branch('main'), $dirtied,
		"and main keeps its dirty working set, $how");
	is(on_branch('other'), $clean, "and another branch its own, $how");
	sql(q{SELECT dolt_reset('--hard')});
	is(on_branch('main'), $clean, "main resets to the commit, $how");
	warm_up();
}

$node->stop;
done_testing();
