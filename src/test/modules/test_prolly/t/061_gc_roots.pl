# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection's roots outside the refs: the value trees hash pointers on
# a database's pages name, dead tuples' too, whatever branch maps the
# page; those named by a transaction that began naming them before the
# collection did (waited for); and those named while it runs (its keep
# set).  A tree nothing names is freed.  A transaction that names a tree
# in a collection's window waits for its swap, and fails if the tree went;
# a prepared one that named one is waited for; a sweep waits for the scan
# of the page store; a collection that fails as it begins lets its keep
# set go.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('roots');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_prepared_transactions = 5
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE EXTENSION injection_points');
sql('CREATE SCHEMA r VERSIONED');
sql('CREATE TABLE r.t (id int PRIMARY KEY, p bytea)');

# a value of its own for each label, and the expression of its pointer
sub value { return "convert_to(repeat('$_[0] ', 20000), 'UTF8')"; }
sub pointer { return 'test_toast_hash_pointer(' . value($_[0]) . ')'; }
sub root { return sql('SELECT (test_toast_pointer(' . pointer($_[0]) . ')).root'); }
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }
# whether the value a row's pointer names reads back whole
sub reads
{
	my ($id, $label) = @_;
	return sql("SELECT test_toast_pointer_value(p) = " . value($label)
		  . " FROM r.t WHERE id = $id");
}
sub gc { return sql('SELECT test_chunk_gc(false) IS NOT NULL'); }

# A tree nothing names is freed; one a row names is kept, and one a dead
# row names; once the dead row is pruned, its tree is freed.
my $unnamed = root('unnamed');
my $named = root('named');
my $dead = root('dead');
sql('INSERT INTO r.t VALUES (1, test_toast_pointer_bytes(' . pointer('named') . '))');
sql('INSERT INTO r.t VALUES (2, test_toast_pointer_bytes(' . pointer('dead') . '))');
sql('DELETE FROM r.t WHERE id = 2');
gc();
is(has($unnamed), 'f', 'a tree no pointer names is freed');
is(has($named), 't', 'a tree a row\'s pointer names is kept');
is(reads(1, 'named'), 't', 'every node of it');
is(has($dead), 't', 'and one a dead row\'s names');
my $pruned = root('pruned');
sql('INSERT INTO r.t VALUES (6, test_toast_pointer_bytes(' . pointer('pruned') . '))');
sql('DELETE FROM r.t WHERE id = 6');
sql('VACUUM r.t');
gc();
is(has($pruned), 'f', 'a tree only a pruned row named is freed');
is(has($named), 't', 'the live row\'s is kept still');

# A branch's rows name trees too: its pages are the page store's.
sql(q{SELECT dolt_commit('-Am', 'named')});
sql(q{SELECT dolt_branch('other')});
my $branched = root('branched');
my $b = $node->background_psql('postgres');
$b->query_safe(q{SELECT dolt_checkout('other')});
$b->query_safe('INSERT INTO r.t VALUES (3, test_toast_pointer_bytes(' . pointer('branched') . '))');
$b->quit;
gc();
is(has($branched), 't', 'a tree another branch\'s row names is kept');

# A transaction that named a tree before the collection began is waited
# for: its row, inserted after, keeps the tree.  (The tree is there
# already, so naming it appends nothing.)
my $waited = root('waited');
my $a = $node->background_psql('postgres');
$a->query_safe('BEGIN');
$a->query_safe('CREATE TEMP TABLE held AS SELECT test_toast_pointer_bytes(' . pointer('waited') . ') AS p');
my $c = $node->background_psql('postgres');
$c->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Lock' AND wait_event = 'transactionid'
	AND query LIKE '%test_chunk_gc%'})
  or die 'the collection did not wait';
pass('a collection waits for a transaction that named a tree');
$a->query_safe('INSERT INTO r.t SELECT 4, p FROM held');
$a->query_safe('COMMIT');
$a->quit;
$c->query_until(qr/collected/, '');
$c->quit;
is(has($waited), 't', 'and keeps the tree its row names');
is(reads(4, 'waited'), 't', 'every node of it');

# A tree named while a collection runs, after it read the pages, is in its
# keep set.
my $kept = root('kept');
my $g = $node->background_psql('postgres');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-roots-scanned', 'wait')});
$g->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-roots-scanned'})
  or die 'the collection did not stop';
sql('INSERT INTO r.t VALUES (5, test_toast_pointer_bytes(' . pointer('kept') . '))');
sql(q{SELECT injection_points_wakeup('prolly-gc-roots-scanned')});
$g->query_until(qr/collected/, '');
$g->quit;
is(has($kept), 't', 'a tree named while a collection runs is kept');
is(reads(5, 'kept'), 't', 'every node of it');

# A tree named in a collection's window, after it last read its keep set:
# naming it waits for the swap, then fails, the swap having freed it.
my $late = root('late');
my $win = $node->background_psql('postgres');
$win->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-kept', 'wait')});
$win->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-kept'})
  or die 'the collection did not stop';
my $namer = $node->background_psql('postgres', on_error_stop => 0);
$namer->query_until(qr/naming/, "\\echo naming\nSELECT (test_toast_pointer("
	  . pointer('late') . ")).root AS late_root;\n\\echo named\n");
sleep(1);
sql(q{SELECT injection_points_wakeup('prolly-gc-kept')});
$win->query_until(qr/collected/, '');
$win->quit;
my $named_out = $namer->query_until(qr/named/, '');
my $late_root = ($named_out =~ /([0-9a-f]{64})/) ? $1 : '';
ok($namer->{stderr} =~ /concurrent collection of the chunk store/
	  || ($late_root ne '' && has($late_root) eq 't'),
	'a tree named in the window fails or is kept, never lost');
$namer->quit;

# A prepared transaction that named a tree is waited for.  (The tree is
# there already, so naming it appends nothing; and a collection first, so
# none is left retiring, whose readers a collection would wait for.)
gc();
my $prep = $node->background_psql('postgres');
$prep->query_safe('SELECT (test_toast_pointer(' . pointer('prepared') . ')).root');
$prep->query_safe('BEGIN');
$prep->query_safe('SELECT (test_toast_pointer(' . pointer('prepared') . ')).root');
$prep->query_safe(q{PREPARE TRANSACTION 'named'});
$prep->quit;
my $pc = $node->background_psql('postgres');
$pc->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Lock' AND wait_event = 'transactionid'
	AND query LIKE '%test_chunk_gc%'})
  or die 'the collection did not wait for the prepared transaction';
pass('a collection waits for a prepared transaction that named a tree');
sql(q{COMMIT PREPARED 'named'});
$pc->query_until(qr/collected/, '');
$pc->quit;

# A sweep waits for a collection's scan of the page store.
my $sc = $node->background_psql('postgres');
$sc->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-scan-store', 'wait')});
$sc->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-scan-store'})
  or die 'the collection did not stop';
my $sw = $node->background_psql('postgres');
$sw->query_until(qr/sweeping/, q{
	\echo sweeping
	SELECT count(*) FROM prolly_page_sweep();
	\echo swept
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event_type = 'Lock' AND query LIKE '%prolly_page_sweep%'})
  or die 'the sweep did not wait';
pass('a sweep waits for a collection\'s scan of the page store');
sql(q{SELECT injection_points_wakeup('bmap-scan-store')});
$sc->query_until(qr/collected/, '');
$sc->quit;
$sw->query_until(qr/swept/, '');
$sw->quit;

# Collections that fail as they begin, more than there are keep sets: the
# next one still runs, and frees a tree nothing names.
sql(q{SELECT injection_points_attach('prolly-gc-sealed', 'error')});
for my $i (1 .. 9)
{
	my ($ret, $out, $err) = $node->psql('postgres',
		'SELECT test_chunk_gc(false) IS NOT NULL');
	die "collection $i: $err" unless $err =~ /error triggered for injection point/;
}
sql(q{SELECT injection_points_detach('prolly-gc-sealed')});
my $after = root('after failures');
gc();
is(has($after), 'f', 'after failed collections, one still frees');

$node->stop;
done_testing();
