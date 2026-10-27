# Copyright (c) 2026, PostgreSQL Global Development Group

# At wal_level=minimal, the maps a relation's storage made from a root
# gets copied (a table unchanged since the root, its free space and
# visibility maps the source's) are logged as images: the copy skips WAL
# there, and no pending sync covers storage made from a root.  So a crash
# that lost or tore the copied pages before a checkpoint wrote them out
# loses nothing: redo writes them again.  The crash here is simulated by
# overwriting the copy's map files, once the server is stopped, with
# garbage that fails checksum verification.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('copied_maps_minimal');
$node->init(extra => ['--data-checksums']);
$node->append_conf('postgresql.conf', q{
wal_level = minimal
max_wal_senders = 0
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# (the last line: a checkout's own output comes first)
sub on_b
{
	my @out = split /\n/, sql(qq{SELECT dolt_checkout('b'); $_[0]});
	return $out[-1];
}

my $summary = q{SELECT all_visible || ',' || all_frozen FROM pg_visibility_map_summary('fv.t')};

sql(q{CREATE EXTENSION pg_visibility;
	CREATE SCHEMA fv VERSIONED;
	CREATE TABLE fv.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL);
	INSERT INTO fv.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 5000) g;
	DELETE FROM fv.t WHERE id % 10 = 0;
	VACUUM (FREEZE) fv.t;
	SELECT dolt_commit('-Am', 'c1')});
sql('CHECKPOINT');
my $n = sql(q{SELECT pg_relation_size('fv.t') / 8192});
is(sql($summary), "$n,$n", 'main\'s table: every page all-visible and all-frozen');

# a branch made of the commit's kept pages: its copy of the table gets
# the source's maps
sql(q{SELECT dolt_branch('b')});
is(on_b($summary), "$n,$n", 'the branch\'s copy has the source\'s map');
my $path = on_b(q{SELECT pg_relation_filepath('fv.t')});
isnt($path, sql(q{SELECT pg_relation_filepath('fv.t')}),
	'the branch\'s copy has storage of its own');
my $datadir = $node->data_dir;
ok(-f "$datadir/${path}_vm", 'its visibility map is a file of its own');
ok(-f "$datadir/${path}_fsm", 'and its free space map');

# a crash before any checkpoint: the copied maps' pages, written only to
# the operating system, torn
$node->stop('immediate');
for my $fork ('_vm', '_fsm')
{
	my $file = "$datadir/$path$fork";
	my $size = -s $file;
	open(my $fh, '+<', $file) or die "open $file: $!";
	binmode $fh;
	print $fh "\xab" x $size;
	close($fh);
}
$node->start;
is(on_b($summary), "$n,$n", 'after the crash, the copy\'s map as it was');
is( on_b(q{SELECT (SELECT count(*) FROM pg_check_frozen('fv.t')) +
		(SELECT count(*) FROM pg_check_visible('fv.t'))}),
	'0', 'and true of the pages');
# (an insert looks for free space in the map)
on_b(q{INSERT INTO fv.t VALUES (100000, 0, 'x')});
unlike(slurp_file($node->logfile), qr/invalid page in block/,
	'no map page failed verification');

$node->stop;
done_testing();
