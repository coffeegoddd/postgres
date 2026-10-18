# Copyright (c) 2026, PostgreSQL Global Development Group

# A transfer over a network remote asks for chunks in batches: a clone
# reads the chunks below each node of a tree in one exchange, and a push
# asks which of them the remote has in one; not a round trip a chunk.  And
# a fetch of commits made since the heads the database has takes one
# exchange, however many there are.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $origin = PostgreSQL::Test::Cluster->new('batch_origin');
$origin->init;
$origin->append_conf('postgresql.conf', qq{autovacuum = off
log_statement = 'all'});
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('batch_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

my $url = 'postgresql:///postgres?host=' . $origin->host . '&port=' . $origin->port;

$origin->safe_psql('postgres', q{
	CREATE SCHEMA b VERSIONED;
	CREATE TABLE b.t (id int PRIMARY KEY, v text);
	INSERT INTO b.t SELECT g, repeat('x', 40) || g FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'many rows');
});

# Calls of a serving function in the origin's log since offset.
sub calls
{
	my ($fn, $offset) = @_;
	my $log = slurp_file($origin->logfile, $offset);
	my @calls = ($log =~ /(?:statement|execute [^:]*): SELECT .*pg_catalog\.$fn\(/g);
	return scalar @calls;
}

$local->safe_psql('postgres', 'CREATE DATABASE c; CREATE DATABASE c2');
$local->safe_psql('c', 'CREATE EXTENSION test_prolly');
my $offset = -s $origin->logfile;
$local->safe_psql('c', qq{SELECT dolt_clone('$url')});
my $chunks = $local->safe_psql('c', 'SELECT sum(chunks) FROM test_chunk_table_files()');
my $gets = calls('dolt_serve_get', $offset);
cmp_ok($chunks, '>', 100, "the clone received many chunks ($chunks)");
cmp_ok($gets, '<', $chunks / 10, "in few requests ($gets)");

# a push of a change to every leaf: which chunks the remote has, asked in
# batches
$local->safe_psql('c', q{
	UPDATE b.t SET v = v || 'y' WHERE id % 50 = 0;
	SELECT dolt_commit('-am', 'changed');
});
$offset = -s $origin->logfile;
$local->safe_psql('c', q{SELECT dolt_push('origin', 'main')});
my $has = calls('dolt_serve_has', $offset);
cmp_ok($has, '<', 60, "the push asked in few requests ($has)");
is($origin->safe_psql('postgres', q{SELECT count(*) FROM b.t WHERE v LIKE '%y'}), '400',
	'and the remote has its rows');

# commits in a line since what the clone has: sent in one exchange, from
# the heads it has, not a round trip a commit
$origin->safe_psql('postgres', q{
	DO $$
	BEGIN
		FOR i IN 1..30 LOOP
			UPDATE b.t SET v = v || 'z' WHERE id = i * 100;
			PERFORM dolt_commit('-am', 'line ' || i);
		END LOOP;
	END $$;
});
$offset = -s $origin->logfile;
$local->safe_psql('c', q{SELECT dolt_pull('origin')});
my $since = calls('dolt_serve_since', $offset);
$gets = calls('dolt_serve_get', $offset);
is($since, 1, 'the new commits asked for once');
cmp_ok($gets, '<=', 2, "and hardly anything after ($gets)");
is($local->safe_psql('c', q{SELECT count(*) FROM b.t WHERE v LIKE '%z'}), '30',
	'pulled all 30 commits\' rows');
is($local->safe_psql('c', q{SELECT dolt_hashof('main')}),
	$origin->safe_psql('postgres', q{SELECT dolt_hashof('main')}), 'at the remote\'s head');

done_testing();
