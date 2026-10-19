# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit is dated when it's made, not at its transaction's start, so one
# made late in a long transaction, on a parent another session committed
# meanwhile, is newer than its parent; and dolt_log lists a commit above
# its parents whatever their dates, by order first, as Dolt's log does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('commit_dates');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA cd VERSIONED;
	CREATE TABLE cd.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'c1');
});

# c3 is made in a transaction that began before c2, its parent, was made.
my $bg = $node->background_psql('postgres');
$bg->query_safe(q{BEGIN; INSERT INTO cd.t VALUES (1)});
sql(q{INSERT INTO cd.t VALUES (2); SELECT dolt_commit('-am', 'c2')});
$bg->query_safe(q{SELECT dolt_commit('-am', 'c3')});
$bg->query_safe(q{COMMIT});
$bg->quit;

is(sql(q{SELECT string_agg(message, ',' ORDER BY ordinality)
	FROM dolt_log('main') WITH ORDINALITY}),
	'c3,c2,c1', 'the log starts at the head');
is(sql(q{SELECT message FROM dolt_log('main') LIMIT 1}), 'c3',
	'which is c3');
is(sql(q{SELECT c3.date > c2.date
	FROM dolt_log('main') c3, dolt_log('main') c2
	WHERE c3.message = 'c3' AND c2.message = 'c2'}),
	't', 'and is newer than its parent');

# Dates and order disagree across branches: b1..b3 are older than m2, but
# b3 and b2 are further from the root.  Dolt's log goes by height first.
sql(q{
	CREATE TABLE cd.u (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('b');
	SELECT dolt_checkout('b');
	INSERT INTO cd.u VALUES (1); SELECT dolt_commit('-am', 'b1');
	INSERT INTO cd.u VALUES (2); SELECT dolt_commit('-am', 'b2');
	INSERT INTO cd.u VALUES (3); SELECT dolt_commit('-am', 'b3');
	SELECT dolt_checkout('main');
	INSERT INTO cd.t VALUES (10); SELECT dolt_commit('-am', 'm2');
	SELECT dolt_merge('b', '-m', 'merge');
});
is(sql(q{SELECT string_agg(message, ',' ORDER BY ordinality)
	FROM dolt_log('main') WITH ORDINALITY WHERE ordinality <= 6}),
	'merge,b3,b2,m2,b1,base', 'the log goes by order before date');

$node->stop;
done_testing();
