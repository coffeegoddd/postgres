# Copyright (c) 2026, PostgreSQL Global Development Group

# A retire keeps what a refs node no longer live still names, while a
# settled file it doesn't collect holds that node: the store holds
# whatever a chunk it holds names.  The refs tree's root and the nodes on
# its path are written anew with every commit and settled by the next
# collection, so every generation holds refs nodes that die at the next
# commit, naming the unchanged nodes beside them, of older generations.
# Here: a refs tree of several leaves (many tags), settled in generation
# A; a commit, settled in B, whose root names A's leaves; then a tag
# dropped, so a leaf of A is no longer live, nor B's root naming it.  The
# retire of A must keep that leaf: B's root, which B's file still holds,
# names it.  Freed, B's root would dangle; and the tag made again writes
# the leaf again, now in a generation newer than B's, which names it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gen_retire_refs_closure');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
prolly.autofold_threshold = 0
});
$node->start;

sub sql { my ($db, $q) = @_; return $node->safe_psql($db, $q); }

my $copies = 0;

# What the store holds that a refs root reaches, walked in a copy whose
# root is moved to it (a chunk named and not held isn't listed).
sub reached_from
{
	my ($db, $root) = @_;
	my $copy = 'walk_' . ++$copies;
	sql('postgres', qq{CREATE DATABASE $copy TEMPLATE $db});
	sql($copy, qq{SELECT test_chunk_move_root(test_chunk_root(), '$root')});
	my @h = sort split /\n/, sql($copy, q{SELECT DISTINCT h FROM test_chunk_reachable() h});
	sql('postgres', qq{DROP DATABASE $copy});
	return \@h;
}

sql('postgres', q{CREATE DATABASE rc});
sql('rc', q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA v VERSIONED;
	CREATE TABLE v.t (id int PRIMARY KEY, rev int NOT NULL);
	INSERT INTO v.t SELECT g, 0 FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'c0')});
# (enough tags for the refs tree to be several leaves under a root)
sql('rc', join("\n", map { sprintf(q{SELECT dolt_tag('tag%04d');}, $_) } 1 .. 600));
sql('rc', q{SELECT * FROM test_chunk_gc(false)});
my $gen_a = sql('rc', q{SELECT test_chunk_generation()});

# a commit: a new root, naming the tags' leaves of A, settled in B
sql('rc', q{UPDATE v.t SET rev = 1 WHERE id = 1; SELECT dolt_commit('-am', 'c1')});
sql('rc', q{SELECT * FROM test_chunk_gc(false)});
my $root_b = sql('rc', q{SELECT test_chunk_root()});
my $before = reached_from('rc', $root_b);
cmp_ok(scalar(@$before), '>', 3, "B's root reaches several chunks");

# a tag dropped: one of A's leaves, and B's root, no longer live
sql('rc', q{SELECT dolt_tag('-d', 'tag0300')});
sql('rc', q{SELECT * FROM test_chunk_gc(false)});
is(sql('rc', qq{SELECT test_chunk_has('$root_b')}), 't',
	"B's root, no longer live, is still held (B's file is)");

sql('rc', qq{SELECT * FROM test_chunk_retire($gen_a, $gen_a)});
is(sql('rc', qq{SELECT test_chunk_has('$root_b')}), 't',
	"B's root is still held after the retire of A");
my $after = reached_from('rc', $root_b);
my %after = map { $_ => 1 } @$after;
my @gone = grep { !$after{$_} } @$before;
is(join(',', @gone), '',
	"the retire of A keeps every chunk B's root, which the store holds, reaches");

# the tag made again: its leaf written again, settled, checked against B's
# root naming it
sql('rc', q{SELECT dolt_tag('tag0300')});
sql('rc', q{SELECT * FROM test_chunk_gc(false)});
my ($ret, $out, $err) = $node->psql('rc', q{SELECT test_chunk_verify_generations()});
is($ret, 0, 'the generations check out once the dropped tag is made again')
  or diag($err);
my ($gret, $gout, $gerr) = $node->psql('rc', q{SELECT * FROM test_chunk_gc(false)});
is($gret, 0, 'and a default collection then succeeds') or diag($gerr);

$node->stop;
done_testing();
