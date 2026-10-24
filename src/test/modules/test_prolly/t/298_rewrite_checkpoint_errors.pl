# Copyright (c) 2026, PostgreSQL Global Development Group

# A table rewrite (ALTER TABLE ... TYPE) checkpoints the session's branch
# by folding it.  That fold covers every table of the branch, so an error
# in it, here made by an injection point, isn't the rewrite's: the ALTER
# succeeds, the skipped checkpoint is logged, and the next commit folds
# what the checkpoint didn't.  A cancel during the checkpoint still
# cancels the ALTER.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('rewrite_checkpoint');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	CREATE TABLE app.other (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT i, 'v' || i FROM generate_series(1, 1000) i;
	INSERT INTO app.other SELECT i, 'o' || i FROM generate_series(1, 100) i;
	SELECT dolt_commit('-Am', 'base')});
sql(q{UPDATE app.other SET v = v || '!' WHERE id <= 10});

my $offset = -s $node->logfile;
my ($rc, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-fold-stamp', 'error');
	ALTER TABLE app.t ALTER COLUMN id TYPE bigint;
});
is($rc, 0, 'a rewrite whose checkpoint fails succeeds');
is($err, '', 'and reports nothing to the client');
like(slurp_file($node->logfile, $offset),
	qr/branch not checkpointed after rewriting "t": error triggered for injection point prolly-fold-stamp/,
	'the skipped checkpoint is logged');
is(sql(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
	WHERE attrelid = 'app.t'::regclass AND attname = 'id'}),
	'bigint', 'the column was retyped');

my $c = sql(q{SELECT (dolt_commit('-am', 'retyped'))[1]});
like($c, qr/^[0-9a-f]{40}$/, 'the next commit folds what the checkpoint did not');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::app.other)}),
	'10', "with the other table's changes");
is(sql(q{SELECT count(*), sum(id) FROM app.t}), '1000|500500', 'and the rewritten rows');

# A cancel in the checkpoint cancels the ALTER: it waits at the same point
# and is cancelled there.
my $alter = $node->background_psql('postgres', on_error_stop => 0);
$alter->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-fold-stamp', 'wait')});
$alter->query_until(qr/started/, q{
	\echo started
	ALTER TABLE app.t ALTER COLUMN v TYPE varchar(200) USING v || '';
	\echo done
});
$node->wait_for_event('client backend', 'prolly-fold-stamp');
sql(q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'prolly-fold-stamp'});
$alter->query_until(qr/done/, '');
like($alter->{stderr}, qr/canceling statement due to user request/, 'a cancel in the checkpoint cancels the rewrite');
$alter->quit;
is(sql(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
	WHERE attrelid = 'app.t'::regclass AND attname = 'v'}),
	'text', 'and the column keeps its type');

$node->stop;
done_testing();
