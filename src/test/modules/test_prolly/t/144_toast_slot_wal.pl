# Copyright (c) 2026, PostgreSQL Global Development Group

# A logical slot decodes a versioned table's rows from the WAL and fetches
# the values they name from the chunk store.  A collection keeps every value
# tree a hash pointer in the WAL names from the oldest position a slot may
# decode from, so a slot behind a collection still decodes a value whose
# rows were deleted and pruned: one a row after the slot's position reused
# (nothing appended after it), and one appended before the slot's position
# (in a segment collected since) that a later row names.  Once the slot is
# gone, they are collected.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('slot_wal');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub value
{
	return "(SELECT string_agg(md5('$_[0]' || i), '' ORDER BY i)
		FROM generate_series(1, 3000) i)";
}
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }
# a full collection: settled table files' chunks too
sub gc { return sql('SELECT test_chunk_gc(true) IS NOT NULL'); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA d VERSIONED');
sql('CREATE TABLE d.docs (id int PRIMARY KEY, t text)');
# (a row that stays, so VACUUM prunes the page rather than cutting it off)
sql(q{INSERT INTO d.docs VALUES (0, 'stays')});

# A value stored, and its segment collected, before the slot is made.
sql('INSERT INTO d.docs VALUES (1, ' . value('reused') . ')');
my $reused = sql('SELECT (test_toast_pointer(t)).root FROM d.docs WHERE id = 1');
ok($reused ne '', 'the value is stored by content');
gc();
is(has($reused), 't', 'a row names it');

# A value stored in a segment sealed before the slot is made (as a long
# transaction may store one), which a row names only after.
my $older = sql('SELECT (test_toast_pointer(test_toast_hash_pointer('
	  . value('older') . '))).root');
sql('SELECT test_chunk_seal()');

sql(q{SELECT pg_create_logical_replication_slot('s', 'test_decoding')});

# After the slot's position: a row reusing the stored value (nothing is
# appended for it), and a row naming the older one; then both rows gone.
sql('INSERT INTO d.docs VALUES (2, ' . value('reused') . ')');
sql('INSERT INTO d.docs VALUES (3, ' . value('older') . ')');
sql('DELETE FROM d.docs WHERE id > 0');
sql('VACUUM d.docs');
gc();
is(has($reused), 't', 'a collection keeps a reused value a slot may still decode');
is(has($older), 't', 'and one stored before the slot\'s position');

my $changes = sql(q{SELECT string_agg(data, E'\n') FROM
	pg_logical_slot_get_changes('s', NULL, NULL, 'include-xids', '0')});
my $v_reused = sql('SELECT ' . value('reused'));
my $v_older = sql('SELECT ' . value('older'));
like($changes, qr/table d\.docs: INSERT: id\[integer\]:2 t\[text\]:'$v_reused'/,
	'the slot decodes the reused value, its rows gone');
like($changes, qr/table d\.docs: INSERT: id\[integer\]:3 t\[text\]:'$v_older'/,
	'and the older one');

# Without the slot, both are collected.
sql(q{SELECT pg_drop_replication_slot('s')});
gc();
is(has($reused), 'f', 'without the slot, the reused value is collected');
is(has($older), 'f', 'and the older one');

$node->stop;
done_testing();
