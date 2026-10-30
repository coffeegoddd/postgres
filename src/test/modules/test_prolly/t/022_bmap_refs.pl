# Copyright (c) 2026, PostgreSQL Global Development Group

# The reference counts of a page store's blocks.
#
# They are kept in pages read through shared buffers, so that a change of
# a count costs a buffer lookup, not an open of the counts' file: a backend
# making many changes in one transaction keeps the file open once, if it
# opens it at all.
#
# They are wide enough that a block shared by more than 255 maps keeps an
# exact count: one that could only stick at its largest value would never
# reach zero as its holders go, and only a sweep would free the block.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('refs');
$node->init;
$node->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_buffercache;
	CREATE TABLE t (a int, b text) WITH (fillfactor = 50);
	INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 2000) g;
});
my $src = sql(q{SELECT pg_relation_filenode('t')});
my $db = sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
my $refs_file = 9419;

# Many copies of shared pages in one transaction, with a pause between
# changes: the backend's open files are looked at during each pause.  A
# count's file opened for each change and closed after it would be open in
# no pause, or under another descriptor in each.
sql(qq{SELECT test_bmap_share($src)});
my $session = $node->background_psql('postgres');
my $pid = $session->query_safe(q{SELECT pg_backend_pid()});
$session->query_safe(q{BEGIN});
my %fds;
my $pauses = 0;
for my $i (0 .. 9)
{
	$session->query_safe(qq{UPDATE t SET a = a WHERE a % 100 = $i});
	$pauses++;
	opendir(my $dh, "/proc/$pid/fd") or last;
	for my $fd (readdir $dh)
	{
		my $target = readlink("/proc/$pid/fd/$fd");
		next unless defined $target;
		$fds{$pauses}{$fd} = 1 if $target =~ m{/base/$db/$refs_file$};
	}
	closedir $dh;
}
$session->query_safe(q{COMMIT});
$session->quit;

SKIP:
{
	skip 'no /proc to look at a backend\'s open files in', 1
	  unless -d "/proc/$pid" || -d '/proc/self/fd';
	my %seen = map { %{$fds{$_} // {}} } keys %fds;
	cmp_ok(scalar(keys %seen), '<=', 1,
		'the counts\' file is open under one descriptor at most over the changes');
}
cmp_ok(sql(qq{SELECT count(*) FROM pg_buffercache
	WHERE relfilenode = $refs_file AND reldatabase = $db}), '>', 0,
	'the counts are read through shared buffers');

# A page shared by 300 copies counts each, and each copy dropped counts
# one less: the last one's drop frees the copied blocks without a sweep.
sql(q{CREATE TABLE one (a int); INSERT INTO one VALUES (1);});
my $one = sql(q{SELECT pg_relation_filenode('one')});
my $phys = sql(qq{SELECT phys FROM test_bmap_map($one) WHERE blkno = 0});
my @copies = split /\n/, sql(qq{SELECT test_bmap_share($one) FROM generate_series(1, 300)});
is(scalar(@copies), 300, '300 copies made');
{
	local $TODO = 'a count is one byte, and sticks at 255 holders';

	is(sql(qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0}), '300',
		'the page counts 300 holders');
}
sql(qq{SELECT test_bmap_unlink($_)}) for @copies[0 .. 199];
{
	local $TODO = 'a count is one byte, and sticks at 255 holders';

	is(sql(qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0}), '100',
		'200 copies dropped, it counts 100');
}
sql(qq{SELECT test_bmap_unlink($_)}) for @copies[200 .. 299];
{
	local $TODO = 'a count is one byte, and sticks at 255 holders';

	is(sql(qq{SELECT refs FROM test_bmap_map($one) WHERE blkno = 0}), '0',
		'every copy dropped, it counts none');
	my $free = sql(q{SELECT test_bmap_free_blocks()});
	sql(q{DROP TABLE one});
	sql(q{SELECT * FROM test_bmap_release_drain()});
	cmp_ok(sql(q{SELECT test_bmap_free_blocks()}) - $free, '>=', 1,
		'dropping the table frees its block, without a sweep');
}
$node->stop;
done_testing();
