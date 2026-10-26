# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits' records are keyed by the commit's id, its hash's bytes, which
# may hold a zero byte.  A publish rebuilt on a root another moved puts
# back the records its transaction made that the rebuilt diff hasn't (one
# equal to the base's): with its key copied whole, not cut at the zero
# byte, so the record stays found under the commit's id.  As in t/396, a
# collection forgets a commit's record and the same commit is made again
# while the collecting transaction is open; here the commit's id is one
# with a zero byte, so this is never left to the hash's luck.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('record_key_zero_byte');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE zb');
$node->safe_psql('zb', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('zb', $_[0]); }

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'base')});
sql(q{SELECT dolt_commit('-Am', 'base')});
my $base = sql(q{SELECT dolt_hashof('HEAD')});

# A commit, made on a branch, whose id has a zero byte: its date tried
# until one gives such an id (about one in thirteen do).
sub make_sql
{
	my ($date) = @_;
	return qq{INSERT INTO s.t VALUES (2, 'again');
	SELECT dolt_commit('-am', 'made again', '--date', '$date') IS NULL;
	SELECT dolt_hashof('HEAD')};
}
sql(qq{SELECT dolt_branch('first', '$base')});
my ($commit, $date);
for my $i (0 .. 599)
{
	my $d = sprintf('2026-01-01T00:%02d:%02dZ', int($i / 60), $i % 60);
	my $id = (split /\n/,
		sql("SELECT dolt_checkout('first') IS NULL; " . make_sql($d)))[-1];
	if ($id =~ /^(?:..)*00/)
	{
		($commit, $date) = ($id, $d);
		last;
	}
	sql(qq{SELECT dolt_checkout('first') IS NULL;
		SELECT dolt_reset('--hard', '$base') IS NULL});
}
ok(defined $commit, 'a commit whose id has a zero byte');
sql(q{SELECT dolt_branch('-D', 'first')});
sql(qq{SELECT dolt_branch('second', '$base')});
my $maker = $node->background_psql('zb');
$maker->query_safe(q{SELECT dolt_checkout('second')});

# A collection takes it and forgets its record; its transaction stays
# open, holding the refs.
my $gc = $node->background_psql('zb');
$gc->query_safe(q{BEGIN});
$gc->query_safe(q{SELECT dolt_gc()});
is(sql(qq{SELECT test_chunk_has('$commit')}), 'f', 'the collection took the commit');

# The same commit made again: its record is the one the base it built on
# still had, and its publish, rebuilt on the collection's root, puts it
# back.
$maker->query_until(qr/started/, "\\echo started\n" . make_sql($date) . ";\n\\echo made\n");
ok($node->poll_query_until('zb', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE state = 'active' AND wait_event_type = 'Lock'
	AND backend_type = 'client backend' AND pid <> pg_backend_pid()}),
	'the commit made again waits for the collection');
$gc->query_safe(q{COMMIT});
$maker->query_until(qr/made/, '');
$maker->quit;
$gc->quit;

is(sql(q{SELECT dolt_hashof('second')}), $commit, 'the commit made again has the same id');
is(sql(qq{SELECT test_refs_record('$commit') IS NOT NULL}), 't',
	'its record is found under its id');

$node->stop;
done_testing();
