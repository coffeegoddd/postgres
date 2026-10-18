# Copyright (c) 2026, PostgreSQL Global Development Group

# Storage freeing many blocks at once: more of one allocator bitmap page's
# blocks than one record can free are freed in several records, and used
# again; recovery replays them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('bmap_free_many');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# Some 17500 blocks, each changed after a commit (its old version copied),
# and then folded again: the fold lets go of the old versions, every one
# at once.  (Three versions of them, and the rest, are well within the
# 65000 or so blocks the allocator's first bitmap page covers.)
sql(q{
	CREATE SCHEMA fm VERSIONED;
	CREATE TABLE fm.big (id int PRIMARY KEY, pad text) WITH (fillfactor = 100);
	ALTER TABLE fm.big ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO fm.big SELECT g, repeat('x', 1900) FROM generate_series(1, 70000) g;
	SELECT dolt_commit('-Am', 'big');
});
my $pages = sql(q{SELECT pg_relation_size('fm.big') / 8192});
cmp_ok($pages, '>', 16384, 'a relation of more blocks than one record frees');
sql(q{UPDATE fm.big SET pad = repeat('y', 1900); SELECT dolt_commit('-am', 'changed')});
sql(q{UPDATE fm.big SET pad = repeat('z', 1900); SELECT dolt_commit('-am', 'again')});
is(sql(q{SELECT count(*) FROM fm.big WHERE pad = repeat('z', 1900)}), '70000',
	'folding after the old versions frees them, the server up');

# A crash replays the frees, and the blocks are used again.
$node->stop('immediate');
$node->start;
sql(q{UPDATE fm.big SET pad = 'w' WHERE id <= 1000; SELECT dolt_commit('-am', 'after')});
is(sql(q{SELECT count(*) FROM fm.big WHERE pad = 'w'}), '1000',
	'a crash replays them, and storage is used again');

$node->stop;
done_testing();
