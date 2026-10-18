# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical decoding of a versioned table's rows whose values are stored by
# content: the decoder fetches each value from the chunk store, a value
# an UPDATE left unchanged too.  A collection keeps the journal's segments
# sealed after the oldest position a logical slot may decode from, so a
# value whose row was deleted and pruned before decoding is still there;
# once the slot is gone, it is collected.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('decoding');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA d VERSIONED');
sql('CREATE TABLE d.docs (id int PRIMARY KEY, n int, t text)');
sql(q{SELECT pg_create_logical_replication_slot('s', 'test_decoding')});

my $value = q{(SELECT string_agg(md5(g || '/' || i), '' ORDER BY i)
	FROM generate_series(1, 3000) i)};
sql("INSERT INTO d.docs SELECT g, 0, $value FROM generate_series(1, 2) g");
sql('UPDATE d.docs SET n = 1 WHERE id = 1');
my $root = sql(q{SELECT (test_toast_pointer(t)).root FROM d.docs WHERE id = 2});
ok($root ne '', 'the values are stored by content');

# The row naming one value deleted and pruned, then the store collected.
sql('DELETE FROM d.docs WHERE id = 2');
sql('VACUUM d.docs');
sql('SELECT test_chunk_gc(false) IS NOT NULL');
is(sql("SELECT test_chunk_has('$root')"), 't',
	'a collection keeps what a slot may still decode');

my $changes = sql(q{SELECT string_agg(data, E'\n') FROM
	pg_logical_slot_get_changes('s', NULL, NULL, 'include-xids', '0')});
my $t1 = sql("SELECT $value FROM (VALUES (1)) v(g)");
my $t2 = sql("SELECT $value FROM (VALUES (2)) v(g)");
like($changes, qr/table d\.docs: INSERT: id\[integer\]:2 n\[integer\]:0 t\[text\]:'$t2'/,
	'an inserted row\'s value is decoded, its row gone');
like($changes, qr/table d\.docs: UPDATE: id\[integer\]:1 n\[integer\]:1 t\[text\]:'$t1'/,
	'and a value an UPDATE left unchanged');
unlike($changes, qr/unchanged-toast-datum/, 'none is left out');

# Without the slot, the value is collected.
sql(q{SELECT pg_drop_replication_slot('s')});
sql('SELECT test_chunk_gc(false) IS NOT NULL');
is(sql("SELECT test_chunk_has('$root')"), 'f', 'without the slot, it is collected');

$node->stop;
done_testing();
