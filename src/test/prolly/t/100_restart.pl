# Copyright (c) 2026, PostgreSQL Global Development Group

# Committed rows survive a clean restart and a crash; rows of a transaction
# that had not committed when the server crashed are gone.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $node = ProllyTest::new_node('restart');
$node->start;

$node->safe_psql('postgres',
	'CREATE TABLE kept (id int PRIMARY KEY, v text);
	 INSERT INTO kept SELECT g, repeat(\'x\', g) FROM generate_series(1, 100) g;');

$node->restart;
is($node->safe_psql('postgres', 'SELECT count(*), sum(length(v)) FROM kept'),
	'100|5050', 'committed rows survive a clean restart');

$node->safe_psql('postgres', 'DELETE FROM kept WHERE id > 50');

# an open transaction's rows, written but not committed when the server
# stops without a shutdown checkpoint
my $open = $node->background_psql('postgres');
$open->query_safe('BEGIN');
$open->query_safe('INSERT INTO kept SELECT g, \'open\' FROM generate_series(1001, 1010) g');

$node->stop('immediate');
$open->{run}->finish;
$node->start;

is($node->safe_psql('postgres', 'SELECT count(*), max(id) FROM kept'),
	'50|50', 'after a crash, committed changes remain and uncommitted ones are gone');

$node->stop;
done_testing();
