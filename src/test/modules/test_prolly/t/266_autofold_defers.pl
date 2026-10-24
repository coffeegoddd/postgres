# Copyright (c) 2026, PostgreSQL Global Development Group

# With the fold worker on, autovacuum's automatic fold (prolly.autofold_threshold)
# leaves branches to it: two folders of one branch would each fold it, one
# losing the race to the other, and autovacuum's checkpoint holds the refs
# through its fold.  With the worker off, autovacuum folds as before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('autofold_defers');
$node->init;
# (the worker on but never started: its launcher sleeps a naptime first)
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
prolly.autofold_threshold = 0.05
prolly_fold_worker = on
prolly_fold_worker_naptime = 600s
log_min_messages = debug1
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t SELECT g, 0 FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'base');
});
my $log = sql(q{SELECT 'pg_toast.pg_prolly_log_' || 's.t'::regclass::oid});

# Written well past the threshold (and autovacuum's least entries), with
# the worker on: autovacuum doesn't fold (several of its passes go by).
my $offset = -s $node->logfile;
sql(q{UPDATE s.t SET v = 1 WHERE id <= 3000});
usleep(5_000_000);
is(sql(qq{SELECT count(*) > 0 FROM $log}), 't',
	'with the fold worker on, autovacuum leaves the branch to it');
unlike(slurp_file($node->logfile, $offset), qr/automatic fold of database/,
	'and logs no automatic fold');

# The worker off: autovacuum folds it.
$node->append_conf('postgresql.conf', 'prolly_fold_worker = off');
$node->reload;
ok( $node->poll_query_until('postgres', qq{SELECT count(*) = 0 FROM $log}),
	'with the fold worker off, autovacuum folds the branch');
like(slurp_file($node->logfile, $offset), qr/automatic fold of database/,
	'as an automatic fold');

done_testing();
