# Copyright (c) 2026, PostgreSQL Global Development Group

# A backend's own edits of the refs carry what its gets read to the root
# the edits make.  An error part way through the carry (out of memory, or
# here an injection point) aborts the transaction, so the root stays the
# one before the edits: what the gets read of it must not hold the edits'
# values then.  The next get, a new transaction's, reads the head the
# aborted commit left, not the one it made.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  if $ENV{enable_injection_points} ne 'yes';

my $node = PostgreSQL::Test::Cluster->new('refs_memo_carry_error');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
fsync = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t VALUES (1, 'a');
	SELECT dolt_commit('-Am', 'load') IS NOT NULL});
my $head = sql(q{SELECT dolt_hashof('main')});

my $s = $node->background_psql('postgres', on_error_stop => 0);
sub rq { my $out = $s->query_safe($_[0]); chomp $out; return $out; }

is(rq(q{SELECT dolt_hashof('main')}), $head, 'the head, read once');
rq(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-refs-memo-carried', 'error')});
$s->query(q{BEGIN;
	INSERT INTO app.t VALUES (2, 'b');
	SELECT dolt_commit('-Am', 'aborted') IS NOT NULL;
	COMMIT});
like($s->{stderr}, qr/prolly-refs-memo-carried/,
	'the carry of the commit\'s edits failed');
$s->{stderr} = '';
rq(q{SELECT injection_points_detach('prolly-refs-memo-carried')});

is(rq(q{SELECT dolt_hashof('main')}), $head,
	'after the abort, the head before the commit read');
is(sql(q{SELECT dolt_hashof('main')}), $head, 'as another session reads it');
is(rq(q{SELECT count(*) FROM app.t}), '1', 'the commit\'s row undone');

$s->quit;
$node->stop;
done_testing();
