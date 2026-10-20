# Copyright (c) 2026, PostgreSQL Global Development Group

# A clone over a libpq connection copies the remote's files whole, unless
# its journal is a large part of it (walked then); and a clone that fails
# partway, tried again, copies only the files it lacks: those it copied
# whole are kept, out of what the store reads, until a clone takes them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $origin = PostgreSQL::Test::Cluster->new('whole_origin');
$origin->init;
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('whole_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

my $url = 'postgresql:///postgres?host=' . $origin->host . '&port=' . $origin->port;
sub o { return $origin->safe_psql('postgres', $_[0]); }
sub l { return $local->safe_psql($_[0], $_[1]); }

# the files of database db's store a clone received whole
my $received = q{SELECT count(*) FROM test_chunk_table_files()
	WHERE name LIKE 'prolly\_received\_%'};

o(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA w VERSIONED;
	CREATE TABLE w.t (id int PRIMARY KEY, v text);
	INSERT INTO w.t SELECT g, md5(g::text) || md5((g + 1)::text) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'one')});
my $head = o(q{SELECT dolt_hashof('main')});

# Its history all in its journal: walked.
l('postgres', 'CREATE DATABASE walked');
l('walked', 'CREATE EXTENSION test_prolly');
l('walked', qq{SELECT dolt_clone('$url')});
is(l('walked', $received), '0', 'a remote whose journal is all of it is walked');
is(l('walked', q{SELECT dolt_hashof('main')}), $head, 'and cloned');

# Collected into files of 64 kB: copied whole.
o(q{SET prolly.gc_file_size = '64kB'; SELECT test_chunk_gc(true)});
my $files = o(q{SELECT count(*) FROM test_chunk_table_files()});
cmp_ok($files, '>', 2, 'the remote has a few files');
l('postgres', 'CREATE DATABASE whole');
l('whole', 'CREATE EXTENSION test_prolly');
l('whole', qq{SELECT dolt_clone('$url')});
is(l('whole', $received), $files, 'a clone copies the remote\'s files whole');
is(l('whole', q{SELECT dolt_hashof('main')}), $head, 'at the remote\'s head');
is(l('whole', q{SELECT count(*), sum(length(v)) FROM w.t}), '3000|192000',
	'with its rows');

# Failed after its first file, then tried again: that file isn't copied
# again.
l('postgres', 'CREATE DATABASE resumed');
l('resumed', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
my ($ret, $out, $err) = $local->psql('resumed', qq{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-clone-file-received', 'error');
	SELECT dolt_clone('$url')});
like($err, qr/error triggered for injection point prolly-clone-file-received/,
	'a clone fails after its first file');
my $dir = $local->data_dir . '/base/'
  . l('resumed', q{SELECT oid FROM pg_database WHERE datname = 'resumed'});
my @kept = glob("$dir/prolly_received_*");
is(scalar(@kept), 1, 'the file it copied is kept');
my $kept_size = -s $kept[0];
is(l('resumed', q{SELECT count(*) FROM test_chunk_table_files()}), '0',
	'and not read');
l('resumed', qq{
	SELECT pg_stat_force_next_flush();
	SELECT pg_stat_reset_shared('prolly');
	SELECT dolt_clone('$url');
	SELECT pg_stat_force_next_flush()});
my $total = l('resumed', q{SELECT sum(bytes) FROM test_chunk_table_files()});
my $written = l('resumed', q{SELECT value FROM pg_stat_prolly
	WHERE name = 'table_file_bytes_written'});
is($written, $total - $kept_size,
	'tried again, it copies only the files it lacked');
is(l('resumed', $received), $files, 'and takes them all');
is(l('resumed', q{SELECT count(*), sum(length(v)) FROM w.t}), '3000|192000',
	'with its rows');

$origin->stop;
$local->stop;
done_testing();
