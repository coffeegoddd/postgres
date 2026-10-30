# Copyright (c) 2026, PostgreSQL Global Development Group

# Storage made of a captured root is refused once the database's oldest
# unfrozen transaction (datfrozenxid) has passed what it was as the root was
# captured: the root's pages may name transactions older than that, unfrozen,
# whose outcome the commit log may have forgotten.  A root captured after is
# accepted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('bmap_root_horizon');
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
	INSERT INTO t SELECT g, repeat('t', 100) FROM generate_series(1, 1000) g;
});
my $t = sql(q{SELECT pg_relation_filenode('t')});
my $old_root = sql(qq{SELECT test_bmap_root_capture($t)});

# Transactions run since, and the whole database frozen: its datfrozenxid
# passes the one the root kept.
my $before = sql(q{SELECT datfrozenxid FROM pg_database WHERE datname = 'postgres'});
sql(q{DO $$ BEGIN FOR i IN 1..200 LOOP PERFORM pg_current_xact_id(); COMMIT; END LOOP; END $$});
sql(q{SET vacuum_freeze_min_age = 0; SET vacuum_freeze_table_age = 0; VACUUM});
my $after = sql(q{SELECT datfrozenxid FROM pg_database WHERE datname = 'postgres'});
is(sql(qq{SELECT age('$before'::xid) > age('$after'::xid)}),
	't', 'the database\'s oldest unfrozen transaction advanced');

my ($ret, $out, $err) =
  $node->psql('postgres', qq{SELECT test_bmap_storage_from_root($old_root)});
isnt($ret, 0, 'storage of the root captured before is refused');
like($err, qr/captured root \d+ is too old to make storage of/,
	'as too old');
like($err, qr/oldest unfrozen transaction ID was \d+, older than \d+ now/,
	'for its transactions');

my $new_root = sql(qq{SELECT test_bmap_root_capture($t)});
my $made = sql(qq{SELECT test_bmap_storage_from_root($new_root)});
is(sql(qq{SELECT test_bmap_rows_digest($made) = test_bmap_rows_digest($t)}),
	't', 'a root captured after is accepted');

$node->stop;
done_testing();
