# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery replays every commit the primary accepted, at the commit
# pipeline's real capacity: with every branch head in use, and after more
# aborted commits to new branches than there is room for pending roots.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('capacity');
$node->init;

# No timed checkpoints: recovery must replay everything below.
$node->append_conf('postgresql.conf', qq(
default_table_access_method = prolly_d1
checkpoint_timeout = '1h'
));
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $usage = q{SELECT heads || ',' || pending FROM prolly_commit_usage('serial')};
my $limit = 1024;

# More aborted commits to new branches than there are pending slots, one
# transaction each.
my $aborts = join '',
  map { "BEGIN; SELECT prolly_commit_test_enqueue('serial', $_); ROLLBACK;\n" }
  (2 .. $limit + 101);
$node->safe_psql('postgres', $aborts);
is($node->safe_psql('postgres', $usage), '0,0',
	'aborted commits leave nothing behind');

# Aborts don't flush the WAL; a commit does, so recovery sees them all.
$node->safe_psql('postgres', 'CREATE TABLE flush_wal (a int) USING heap');

$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', 'CHECKPOINT');
is($node->safe_psql('postgres', $usage), '0,0',
	'recovery replays more aborted commits than there are pending slots');

# Every head in use, then an update of one branch that has a head.
for (my $first = 2; $first < $limit + 2; $first += 128)
{
	my $last = $first + 127;
	$last = $limit + 1 if $last > $limit + 1;
	$node->safe_psql('postgres',
		"SELECT prolly_commit_test_enqueue('serial', g) FROM generate_series($first, $last) g");
}
is($node->safe_psql('postgres', $usage), "$limit,0", 'every head is in use');

my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"SELECT prolly_commit_test_enqueue('serial', " . ($limit + 2) . ")");
like($stderr, qr/too many prolly branch heads/, 'a new branch is refused');
$node->safe_psql('postgres', "SELECT prolly_commit_test_enqueue('serial', 2)");

$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', 'CHECKPOINT');
is($node->safe_psql('postgres', $usage), "$limit,0",
	'recovery replays an update with every head in use');

# The update after recovery still works, and survives a clean restart.
$node->safe_psql('postgres', "SELECT prolly_commit_test_enqueue('serial', 3)");
$node->restart;
is($node->safe_psql('postgres', $usage), "$limit,0", 'and a restart');

done_testing();
