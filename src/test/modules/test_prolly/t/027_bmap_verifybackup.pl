# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_basebackup and pg_verifybackup accept a cluster with block-mapped
# tables: a base backup of tables that share pages verifies against its
# manifest, in plain and tar format; pg_verifybackup finds a page store
# file changed after the backup; and the backup starts with the tables'
# rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('verify_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
});
$primary->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $copy = sql(qq{SELECT test_bmap_share($t)});
my $copy_rows = sql(qq{SELECT test_bmap_rows_digest($copy)});
sql(q{UPDATE t SET b = 'y' WHERE a % 3 = 0});
my $rows_query = q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM t};
my $rows = sql($rows_query);
my $db = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});

my $plain = $primary->backup_dir . '/plain';
$primary->command_ok(
	[ 'pg_basebackup', '--pgdata' => $plain, '--no-sync', '--checkpoint' => 'fast' ],
	'a plain base backup is taken');
command_ok([ 'pg_verifybackup', $plain ], 'and verifies');

my $tar = $primary->backup_dir . '/tar';
$primary->command_ok(
	[ 'pg_basebackup', '--pgdata' => $tar, '--format' => 'tar', '--no-sync',
	  '--checkpoint' => 'fast' ],
	'a tar base backup is taken');
command_ok([ 'pg_verifybackup', '--no-parse-wal', $tar ], 'and verifies');

# A page store file changed after the backup is found.
my $store = "$plain/base/$db/9421";
ok(-f $store, 'the backup has the page store');
open(my $fh, '+<', $store) or die "could not open $store: $!";
binmode $fh;
my $orig;
sysseek($fh, 8192 + 200, 0);
sysread($fh, $orig, 4);
sysseek($fh, 8192 + 200, 0);
syswrite($fh, "\xde\xad\xbe\xef");
close $fh;
command_fails_like([ 'pg_verifybackup', $plain ], qr/checksum mismatch/,
	'a changed page store file is found');
open($fh, '+<', $store) or die "could not open $store: $!";
binmode $fh;
sysseek($fh, 8192 + 200, 0);
syswrite($fh, $orig);
close $fh;
command_ok([ 'pg_verifybackup', $plain ], 'and verifies again once restored');

# The backup starts with the rows as they were.
my $restored = PostgreSQL::Test::Cluster->new('verify_restored');
$restored->init_from_backup($primary, 'plain');
$restored->start;
is($restored->safe_psql('postgres', $rows_query), $rows,
	'the restored table has its rows');
is($restored->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($copy)}),
	$copy_rows, 'and the copy its own');
$restored->stop;

$primary->stop;
done_testing();
