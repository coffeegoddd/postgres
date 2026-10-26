# Copyright (c) 2026, PostgreSQL Global Development Group

# The step after a collection that forgets collected commits' relation
# records holds the refs, and asks again under them, so a commit made
# again meanwhile (the same id: same parent, change, date and message) by
# another session keeps its record: its publish waits for the collecting
# transaction and puts the record back over the deletion.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('forget_records_concurrent');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE fr');
$node->safe_psql('fr', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('fr', $_[0]); }

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'base')});
sql(q{SELECT dolt_commit('-Am', 'base')});
my $base = sql(q{SELECT dolt_hashof('HEAD')});

# The commit made on a branch, which is then deleted.
my $make = q{INSERT INTO s.t VALUES (2, 'again');
	SELECT dolt_commit('-am', 'made again', '--date', '2026-01-01T00:00:00Z') IS NULL;
	SELECT dolt_hashof('HEAD')};
sql(qq{SELECT dolt_branch('first', '$base')});
my $commit = (split /\n/, sql("SELECT dolt_checkout('first') IS NULL; $make"))[-1];
sql(q{SELECT dolt_branch('-D', 'first')});
sql(qq{SELECT dolt_branch('second', '$base')});
my $maker = $node->background_psql('fr');
$maker->query_safe(q{SELECT dolt_checkout('second')});

# A collection takes it, and its transaction stays open.
my $gc = $node->background_psql('fr');
$gc->query_safe(q{BEGIN});
$gc->query_safe(q{SELECT dolt_gc()});
is(sql(qq{SELECT test_chunk_has('$commit')}), 'f', 'the collection took the commit');

# Another session makes the same commit again; its publish waits for the
# collecting transaction.
$maker->query_until(qr/started/, "\\echo started\n$make;\n\\echo made\n");
# (it waits on the refs the collecting transaction holds; were they not
# held, it would publish at once)
$node->poll_query_until('fr', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE state = 'active' AND wait_event_type = 'Lock'
	AND backend_type = 'client backend' AND pid <> pg_backend_pid()})
  or diag('the second commit did not wait');
$gc->query_safe(q{COMMIT});
$maker->query_until(qr/made/, '');
$maker->quit;
$gc->quit;

is(sql(q{SELECT dolt_hashof('second')}), $commit, 'the commit made again has the same id');
is(sql(qq{SELECT test_chunk_has('$commit')}), 't', 'it is stored');
is(sql(qq{SELECT test_refs_record('$commit') IS NOT NULL}), 't',
	'its record survives the collection that forgot it before');

$node->stop;
done_testing();
