# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection waits for no prepared transaction, which may stay for days:
# not at its retire for one running at its swap, whether or not it used
# the store; not for one that appended to the segments it collects, whose
# chunks it keeps as roots; nor for one that named a tree before it began,
# whose pointers are on pages.  One it waits for that is prepared
# meanwhile is waited for no more.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gc_prepared');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_prepared_transactions = 5
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# a collection (full, with a true argument) that doesn't finish fails
# rather than hangs
sub gc
{
	my $full = $_[0] ? 'true' : 'false';
	return sql(qq{SET statement_timeout = '60s';
		SELECT kept || ' ' || freed FROM test_chunk_gc($full)});
}

sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA r VERSIONED');
sql('CREATE TABLE r.t (id int PRIMARY KEY, p bytea)');
sql('CREATE TABLE h (a int) USING heap');
sql(q{SELECT dolt_commit('-Am', 'x')});

# a value of its own for each label, and the expression of its pointer
sub value { return "convert_to(repeat('$_[0] ', 20000), 'UTF8')"; }
sub pointer { return 'test_toast_hash_pointer(' . value($_[0]) . ')'; }

# how many of n chunks labelled so the store has
sub chunks
{
	my ($label, $n) = @_;
	return "SELECT test_chunk_put(convert_to(repeat(md5(g::text), 1000) || '$label ' || g, 'UTF8'))
		FROM generate_series(1, $n) g";
}
sub has
{
	my ($label, $n) = @_;
	return sql("SELECT count(*) FROM generate_series(1, $n) g
		WHERE test_chunk_has(encode(sha256(convert_to(repeat(md5(g::text), 1000) || '$label ' || g, 'UTF8')), 'hex'))");
}

# The retire: a prepared transaction that never used the store, running
# at the swap, isn't waited for.
sql(q{BEGIN; INSERT INTO h VALUES (1); PREPARE TRANSACTION 'heap'});
is(sql(q{SET statement_timeout = '60s'; SELECT dolt_gc()}), '{0}',
	'dolt_gc() finishes with a prepared transaction running at its swap');
sql(q{COMMIT PREPARED 'heap'});

# The window: one that appended to the segments collected isn't waited
# for, its chunks kept; once it is finished they are its own, or garbage.
sql(q{BEGIN; } . chunks('kept', 20) . q{; PREPARE TRANSACTION 'kept'});
sql(q{BEGIN; } . chunks('gone', 20) . q{; PREPARE TRANSACTION 'gone'});
like(gc(), qr/^\d+ \d+$/,
	'a collection finishes with prepared transactions that appended chunks');
sql(q{COMMIT PREPARED 'kept'});
sql(q{ROLLBACK PREPARED 'gone'});
is(has('kept', 20), '20', 'a committed prepared transaction\'s chunks are kept');
gc(1);
is(has('gone', 20), '0', 'a rolled back one\'s are collected by a full one');

# One waited for (its chunks appended, in progress at the seal), then
# prepared: the collection goes on, its chunks kept.
my $late = $node->background_psql('postgres');
$late->query_safe('BEGIN');
$late->query_safe(chunks('late', 50));	# (more than an append's batch)
my $g = $node->background_psql('postgres');
$g->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Lock' AND query LIKE '%test_chunk_gc%'})
  or die 'the collection did not wait for the transaction';
$late->query_safe(q{PREPARE TRANSACTION 'late'});
$g->query_until(qr/collected/, '');
pass('a collection waiting for a transaction that appended goes on once it is prepared');
sql(q{COMMIT PREPARED 'late'});
is(has('late', 50), '50', 'its chunks are kept');

# One waited for (it named a tree before the collection began, in a row),
# then prepared: the collection goes on, and the tree the row names is
# kept.  (The tree is there already, so naming it appends nothing.)
sql('SELECT (test_toast_pointer(' . pointer('named') . ')).root');
my $named = $node->background_psql('postgres');
$named->query_safe('BEGIN');
$named->query_safe('INSERT INTO r.t VALUES (1, test_toast_pointer_bytes('
	  . pointer('named') . '))');
$g->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Lock' AND query LIKE '%test_chunk_gc%'})
  or die 'the collection did not wait for the transaction';
$named->query_safe(q{PREPARE TRANSACTION 'named'});
$g->query_until(qr/collected/, '');
pass('a collection waiting for a transaction that named a tree goes on once it is prepared');
gc();
sql(q{COMMIT PREPARED 'named'});
is(sql('SELECT test_toast_pointer_value(p) = ' . value('named')
	  . ' FROM r.t WHERE id = 1'),
	't', 'the tree its row names reads back whole');

$late->quit;
$named->quit;
$g->quit;
done_testing();
