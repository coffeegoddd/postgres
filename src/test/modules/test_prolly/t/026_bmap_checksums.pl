# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_checksums accepts a cluster with block-mapped tables.  The page
# store's pages carry checksums of their logical block numbers, which a
# file's block numbers don't give, so it leaves the store out of its check
# (the server verifies those pages as it reads them); it checks the map
# forks and every other file as it would.  Enabling checksums is refused:
# it would compute the store's checksums with the wrong block numbers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('checksums');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE TABLE t (a int, b text);
	INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
	CREATE INDEX ON t (a);
	UPDATE t SET b = 'y' WHERE a % 3 = 0;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $copy = sql(qq{SELECT test_bmap_share($t)});
sql(q{UPDATE t SET b = 'z' WHERE a % 5 = 0});
my $rows = sql(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM t});
my $datadir = $node->data_dir;
my $db = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
ok(-f "$datadir/base/$db/${t}_bmap", 'the table is block-mapped');
$node->stop;

command_ok([ 'pg_checksums', '--check', '--pgdata' => $datadir ],
	'pg_checksums checks the cluster');
command_like([ 'pg_checksums', '--check', '--pgdata' => $datadir ],
	qr/Bad checksums:\s+0/, 'and finds no bad checksum');

# A map fork corrupted is found.
my $bmap = "$datadir/base/$db/${t}_bmap";
open(my $fh, '+<', $bmap) or die "could not open $bmap: $!";
binmode $fh;
my $orig;
sysseek($fh, 100, 0);
sysread($fh, $orig, 4);
sysseek($fh, 100, 0);
syswrite($fh, "\xde\xad\xbe\xef");
close $fh;
command_fails_like([ 'pg_checksums', '--check', '--pgdata' => $datadir ],
	qr/checksum verification failed/, 'a corrupt map page is found');
open($fh, '+<', $bmap) or die "could not open $bmap: $!";
binmode $fh;
sysseek($fh, 100, 0);
syswrite($fh, $orig);
close $fh;
command_ok([ 'pg_checksums', '--check', '--pgdata' => $datadir ],
	'and the cluster checks again once restored');

# Turning checksums off works; turning them on again is refused.
command_ok([ 'pg_checksums', '--disable', '--pgdata' => $datadir ],
	'pg_checksums turns checksums off');
command_fails_like([ 'pg_checksums', '--enable', '--pgdata' => $datadir ],
	qr/block-mapped/, 'and refuses to turn them on with block-mapped storage');

$node->start;
is(sql(q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM t}), $rows,
	'the table\'s rows are intact');
is(sql(q{SELECT count(*) FROM t WHERE a < 100}), '99', 'and its index works');
$node->stop;
done_testing();
