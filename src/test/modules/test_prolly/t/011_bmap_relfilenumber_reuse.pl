# Copyright (c) 2026, PostgreSQL Global Development Group

# A block-mapped relation's main fork has no file of its own, only its map
# fork.  A new relation must never be given a block-mapped relation's file
# number, whatever storage manager it is made in: smgropen() would find the
# map fork and take the other relation's storage for the new one's.  The
# OID counter is set back to a block-mapped table's file number, as a
# wraparound would bring it, and a new table must get another.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reuse');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	SET debug_block_mapped_heap = on;
	CREATE TABLE t (a int);
	TRUNCATE t;
	INSERT INTO t SELECT generate_series(1, 10);
});
my $t_relfilenode = sql(q{SELECT relfilenode FROM pg_class WHERE relname = 't'});
ok(-f $node->data_dir . '/' . sql(q{SELECT pg_relation_filepath('t')}) . '_bmap',
	'the table is block-mapped');

$node->stop;
command_ok([ 'pg_resetwal', '-o', $t_relfilenode, $node->data_dir ],
	'the OID counter is set back to the table\'s file number');
$node->start;

sql(q{CREATE TABLE u (a int)});
my $u_relfilenode = sql(q{SELECT relfilenode FROM pg_class WHERE relname = 'u'});
isnt($u_relfilenode, $t_relfilenode,
	'a new table does not take a block-mapped table\'s file number');

sql(q{INSERT INTO u VALUES (999)});
is(sql(q{SELECT count(*) || ':' || max(a) FROM t}), '10:10',
	'the block-mapped table keeps its own rows');
is(sql(q{SELECT count(*) || ':' || max(a) FROM u}), '1:999',
	'and the new table its own');

$node->stop;
done_testing();
