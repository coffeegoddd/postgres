# Copyright (c) 2026, PostgreSQL Global Development Group

# A clone that copies a remote's files whole reads each received file's
# index as it goes.  One that isn't there whole (cut short) is passed
# over, and never adopted; any other error opening one (here an injection
# point's) says nothing of the file, and is raised: the file is kept, not
# taken for one cut short, nor left out of what the backend lists, so the
# clone tried again in the same session takes it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $origin = PostgreSQL::Test::Cluster->new('received_origin');
$origin->init;
$origin->append_conf('postgresql.conf', qq{autovacuum = off
prolly.gc_file_size = '1MB'});
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('received_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

my $url = 'postgresql:///postgres?host=' . $origin->host . '&port=' . $origin->port;
sub o { return $origin->safe_psql('postgres', $_[0]); }
sub l { return $local->safe_psql($_[0], $_[1]); }
my $received = q{SELECT count(*) FROM test_chunk_table_files()
	WHERE name LIKE 'prolly\_received\_%'};

o(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA w VERSIONED;
	CREATE TABLE w.t (id int PRIMARY KEY, v text);
	INSERT INTO w.t SELECT g, md5(g::text) || md5((g + 1)::text) FROM generate_series(1, 60000) g;
	SELECT dolt_commit('-Am', 'one');
	SELECT test_chunk_gc(true)});
my $head = o(q{SELECT dolt_hashof('main')});
my $files = o(q{SELECT count(*) FROM test_chunk_table_files()});
cmp_ok($files, '>', 2, 'the remote has a few files');

l('postgres', 'CREATE DATABASE c');
l('c', 'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
my ($ret, $out, $err) = $local->psql('c', qq{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-received-open', 'error');
	SELECT dolt_clone('$url');
	SELECT injection_points_detach('prolly-received-open');
	SELECT 'second ' || (dolt_clone('$url') IS NOT NULL);
	SELECT 'head ' || dolt_hashof('main');
}, on_error_stop => 0);
like($err, qr/ERROR:  error triggered for injection point prolly-received-open/,
	'an error opening a received file fails the clone');
like($out, qr/second true/, 'the clone tried again in the same session succeeds');
like($out, qr/head \Q$head\E/, 'at the remote\'s head');
# (the remote's table files, by size, among those received; its journal,
# if it has one, is received too)
my $sizes = q{SELECT string_agg(bytes::text, ' ' ORDER BY bytes) FROM test_chunk_table_files()};
my %got = map { $_ => 1 } split / /, l('c', $sizes);
is(scalar(grep { !$got{$_} } split / /, o($sizes)), 0, 'taking every file whole');
is(l('c', q{SELECT count(*), sum(length(v)) FROM w.t}), '60000|3840000',
	'with its rows');

$origin->stop;
$local->stop;
done_testing();
