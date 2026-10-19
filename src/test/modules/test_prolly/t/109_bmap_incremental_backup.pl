# Copyright (c) 2026, PostgreSQL Global Development Group

# Incremental backups of block-mapped storage.  WAL names the logical blocks
# of block-mapped relations, never the page store's physical blocks, so WAL
# summaries know nothing of the page store's changes: an incremental backup
# must send the page store in full, and pg_combinebackup must refuse one
# sent incrementally rather than restore it as the prior backup had it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
summarize_wal = on
autovacuum = off
});
$primary->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

my $dboid = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $digest = q{
	SELECT count(*) || ':' || count(*) FILTER (WHERE b = 'changed') || ':'
		   || md5(string_agg(a || b, ',' ORDER BY a))
	FROM h};

sql(q{
	CREATE TABLE h (a int PRIMARY KEY, b text);
	INSERT INTO h SELECT g, 'orig' FROM generate_series(1, 50000) g;
});
ok(-f $primary->data_dir . '/' . sql(q{SELECT pg_relation_filepath('h')}) . '_bmap',
	'the table is block-mapped');

my $backup_path = $primary->backup_dir;
$primary->backup('full');

# Change existing pages and add new ones, so the page store both changes in
# place and grows.
sql(q{
	UPDATE h SET b = 'changed' WHERE a % 2 = 0;
	INSERT INTO h SELECT g, 'new' FROM generate_series(50001, 60000) g;
	CHECKPOINT;
});
my $expected = sql($digest);
like($expected, qr/^60000:25000:/, 'the source has the changes');

$primary->backup('incr',
	backup_options => [ '--incremental', "$backup_path/full/backup_manifest" ]);

my $incr_db = "$backup_path/incr/base/$dboid";
ok(!-e "$incr_db/INCREMENTAL.9421",
	'the page store is not sent incrementally');
ok(-f "$incr_db/9421", 'the page store is sent in full');
my @incremental = glob("$incr_db/INCREMENTAL.*");
cmp_ok(scalar(@incremental), '>', 0,
	'other files are sent incrementally');

my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup($primary, 'incr', combine_with_prior => ['full']);
$restored->start;
is($restored->safe_psql('postgres', $digest), $expected,
	'the combined backup has every change since the full backup');

# The index, from the incremental backup's files, finds the rows in the
# page store as the combined backup has it.  No statistics after the
# restore: force the index scan rather than trust the planner to choose it.
my $by_index = q{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexonlyscan = off;
	EXPLAIN (COSTS OFF)
	SELECT string_agg(a || b, ',' ORDER BY a) FROM h
	WHERE a BETWEEN 49990 AND 50010;
	SELECT string_agg(a || b, ',' ORDER BY a) FROM h
	WHERE a BETWEEN 49990 AND 50010};
my $indexed = $restored->safe_psql('postgres', $by_index);
like($indexed, qr/Index Scan using h_pkey on h/, 'the scan is by the index');
is((split /\n/, $indexed)[-1], (split /\n/, sql($by_index))[-1],
	'the index finds the rows');
$restored->stop;

# An incremental backup that sent the page store incrementally, as a server
# without the fix did, is refused, whichever segment (or fork, which the
# store has none of) the incremental file is of.  The check comes before
# reconstruction reads the file, so any incremental file's contents do.
my $forged = "$backup_path/forged";
PostgreSQL::Test::RecursiveCopy::copypath("$backup_path/incr", $forged);
my $forged_db = "$forged/base/$dboid";
unlink("$forged_db/9421") or die "could not remove page store: $!";
my $sample = (glob("$forged_db/INCREMENTAL.*"))[0];
foreach my $name ('9421', '9421.1', '9421_vm')
{
	PostgreSQL::Test::Utils::append_to_file("$forged_db/INCREMENTAL.$name",
		PostgreSQL::Test::Utils::slurp_file($sample));
	command_fails_like(
		[
			'pg_combinebackup', "$backup_path/full", $forged,
			'--output' => PostgreSQL::Test::Utils::tempdir . '/forged_out',
		],
		qr/"[^"]*INCREMENTAL\.\Q$name\E" is an incremental copy of a block-mapped page store(?s:.*)hint: Take a new full backup/,
		"pg_combinebackup refuses INCREMENTAL.$name");
	unlink("$forged_db/INCREMENTAL.$name") or die "could not remove: $!";
}

$primary->stop;

done_testing();
