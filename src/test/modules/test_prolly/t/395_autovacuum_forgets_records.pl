# Copyright (c) 2026, PostgreSQL Global Development Group

# Autovacuum's automatic collection is followed, as a step of its own, by
# the refs forgetting the relation records of the commits it took, as
# dolt_gc() is: a record isn't a reference the collection follows, so it
# would outlive its commit.  A live commit's record stays.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('autovac_records');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.gc_journal_threshold = 1MB
prolly.gc_growth_scale_factor = 0
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE av');
$node->safe_psql('av', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('av', $_[0]); }

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'live')});
sql(q{SELECT dolt_commit('-Am', 'live')});
my $live = sql(q{SELECT dolt_hashof('HEAD')});
sql(q{SELECT dolt_branch('abandon')});
my $abandoned = sql(q{SELECT dolt_checkout('abandon') IS NULL;
	INSERT INTO s.t VALUES (2, 'abandoned');
	SELECT dolt_commit('-am', 'to abandon') IS NULL;
	SELECT dolt_hashof('HEAD')});
$abandoned = (split /\n/, $abandoned)[-1];
sql(q{SELECT dolt_branch('-D', 'abandon')});
is(sql(qq{SELECT test_refs_record('$abandoned') IS NOT NULL}),
	't', 'the abandoned commit is recorded');

# Past the threshold: an automatic collection takes the abandoned commit,
# and its record goes after it.
my $log_offset = -s $node->logfile;
sql(q{SELECT count(test_chunk_put(test_noise(g::text, 100000)))
	FROM generate_series(1, 20) g});
ok( $node->poll_query_until(
		'av', qq{SELECT NOT test_chunk_has('$abandoned')
		AND test_refs_record('$abandoned') IS NULL}),
	"an automatic collection's step after it forgets the collected commit's record");
is(sql(qq{SELECT test_refs_record('$live') IS NOT NULL}),
	't', "a live commit's record stays");
is(sql(q{SELECT count(*) FROM s.t}), '1', 'the table reads');

my $log = slurp_file($node->logfile, $log_offset);
my @errors = grep { /autovacuum worker.*ERROR/ } split /\n/, $log;
is(scalar(@errors), 0, 'the automatic collections met no error')
	or diag(join("\n", @errors));

$node->stop;
done_testing();
