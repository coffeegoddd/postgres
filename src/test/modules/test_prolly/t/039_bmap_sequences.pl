# Copyright (c) 2026, PostgreSQL Global Development Group

# Sequences are made in block-mapped storage as tables are: a permanent
# sequence that isn't a catalog's, with debug_block_mapped_heap; unlogged
# and temporary ones stay in md.c.  A sequence's storage shares as a
# table's does: nextval() on the source, which changes the sequence's page
# in place, copies the page first, so the copy keeps the value it was
# shared at, through a crash and on a standby.  ALTER SEQUENCE's new
# storage stays block-mapped.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('sequences');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
debug_block_mapped_heap = on
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->safe_psql('postgres', q{CREATE EXTENSION test_prolly});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('sequences_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

sub sql { return $primary->safe_psql('postgres', $_[0]); }
sub ssql { return $standby->safe_psql('postgres', $_[0]); }
sub filenode { return sql(qq{SELECT pg_relation_filenode('$_[0]')}); }

sql(q{
	CREATE SEQUENCE s;
	CREATE UNLOGGED SEQUENCE u;
	CREATE TABLE t (id bigserial, v int);
	SELECT nextval('s') FROM generate_series(1, 10);
});
my $s = filenode('s');
is(sql(qq{SELECT test_bmap_exists($s)}), 't', 'a sequence is block-mapped');
is(sql(qq{SELECT test_bmap_exists(@{[filenode('t_id_seq')]})}), 't',
	'and so is a serial column\'s');
is(sql(qq{SELECT test_bmap_exists(@{[filenode('u')]})}), 'f',
	'an unlogged sequence is not');
is(sql(q{SELECT test_bmap_exists(pg_relation_filenode('pg_catalog.pg_class'))}), 'f',
	'nor is a catalog');

# Shared at 10: the source's nextval() copies the page, the copy keeps 10.
my $copy = sql(qq{SELECT test_bmap_share($s)});
my $shared_at = sql(qq{SELECT test_bmap_rows_digest($copy)});
my $phys = sql(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 0});
is(sql(q{SELECT nextval('s')}), '11', 'the source goes on from where it was');
sql(q{SELECT nextval('s') FROM generate_series(1, 40)});
isnt(sql(qq{SELECT phys FROM test_bmap_map($s) WHERE blkno = 0}), $phys,
	'its page was copied before it changed');
is(sql(qq{SELECT phys FROM test_bmap_map($copy) WHERE blkno = 0}), $phys,
	'the copy keeps the shared block');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $shared_at,
	'and reads the sequence as it was shared');
isnt(sql(qq{SELECT test_bmap_rows_digest($s)}), $shared_at,
	'the source reads its own');

$primary->wait_for_replay_catchup($standby);
is(ssql(qq{SELECT test_bmap_rows_digest($copy)}), $shared_at,
	'the standby reads the copy as shared');
# (a standby reads the value nextval() logged ahead, as it does in md.c)
cmp_ok(ssql(q{SELECT last_value FROM s}), '>=', sql(q{SELECT last_value FROM s}),
	'and the source past every value it gave out');

# A crash: the source goes on past every value it gave out, the copy
# still reads as shared.
my $source = sql(q{SELECT test_bmap_rows_digest(pg_relation_filenode('s'))});
$primary->stop('immediate');
$primary->start;
cmp_ok(sql(q{SELECT nextval('s')}), '>', 51,
	'after a crash the source gives out no value again');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $shared_at,
	'and the copy reads as shared');

# ALTER SEQUENCE ... RESTART gives the sequence new storage, block-mapped.
sql(q{ALTER SEQUENCE s RESTART WITH 1000});
my $s2 = filenode('s');
isnt($s2, $s, 'a restart makes new storage');
is(sql(qq{SELECT test_bmap_exists($s2)}), 't', 'which is block-mapped');
is(sql(q{SELECT nextval('s')}), '1000', 'and reads the restarted value');
is(sql(qq{SELECT test_bmap_rows_digest($copy)}), $shared_at,
	'the copy is left as shared');

# Rows of a serial column in a shared table's sequence: inserts after a
# share go on from the source's value.
sql(q{INSERT INTO t (v) SELECT g FROM generate_series(1, 5) g});
my $seq_copy = sql(qq{SELECT test_bmap_share(@{[filenode('t_id_seq')]})});
sql(q{INSERT INTO t (v) VALUES (6)});
is(sql(q{SELECT max(id) FROM t}), '6', 'a serial column goes on after its sequence is shared');
$primary->wait_for_replay_catchup($standby);
is(ssql(q{SELECT max(id) FROM t}), '6', 'on the standby too');

sql(qq{SELECT test_bmap_unlink($copy)});
sql(qq{SELECT test_bmap_unlink($seq_copy)});
done_testing();
