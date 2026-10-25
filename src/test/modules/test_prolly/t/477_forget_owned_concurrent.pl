# Copyright (c) 2026, PostgreSQL Global Development Group

# A record names its routines and types through an entry records share,
# which a commit finding it stored already doesn't write.  The step after a
# collection forgets those no record left names, so one may go between a
# transaction's commit finding it and the transaction's publish: the
# publish, rebuilt on the root moved meanwhile, puts back the entry its
# record names.  (The entry is deleted here directly, as that step would.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('forget_owned_concurrent');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE fo');
$node->safe_psql('fo', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('fo', $_[0]); }

sub owned_list
{
	my ($commit) = @_;
	return sql(
		qq{SELECT substring(convert_from(test_refs_value('L' || '$commit'), 'UTF8')
			FROM '\@([0-9a-f]{64})\\n\$')});
}

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'base');
	CREATE FUNCTION s.f1() RETURNS int LANGUAGE sql AS 'SELECT 1'});
sql(q{SELECT dolt_commit('-Am', 'base')});
sql(q{SELECT dolt_branch('y')});

my $maker = $node->background_psql('fo');
$maker->query_safe(q{SELECT dolt_checkout('y')});
$maker->query_safe(
	q{CREATE FUNCTION s.f2() RETURNS int LANGUAGE sql AS 'SELECT 2'});
$maker->query_safe(q{SELECT dolt_commit('-Am', 'f2')});
my $list = owned_list(sql(q{SELECT dolt_hashof('y')}));
isnt($list, '', 'the commit names its routines by an entry');

# The next commit's record names the same entry, found stored, so it isn't
# written; the transaction hasn't published yet.
$maker->query_safe(q{BEGIN});
$maker->query_safe(q{UPDATE s.t SET v = 'on y'});
$maker->query_safe(q{SELECT dolt_commit('-am', 'y')});

# Meanwhile the entry is deleted.
is(sql(qq{SELECT test_refs_delete('J' || '$list')}), 't', 'the entry is deleted');
is(sql(qq{SELECT test_refs_value('J' || '$list') IS NULL}), 't', 'it is gone');

$maker->query_safe(q{COMMIT});
$maker->quit;

my $y = sql(q{SELECT dolt_hashof('y')});
is(owned_list($y), $list, "y's commit names the entry");
is(sql(qq{SELECT test_refs_value('J' || '$list') IS NOT NULL}),
	't', 'its publish put the entry back');

# A move reading the record finds every routine's owner.
is( sql(
		qq{SELECT dolt_checkout('y') IS NULL; DROP FUNCTION s.f2();
		SELECT dolt_reset('--hard', '$y') IS NOT NULL;
		SELECT to_regprocedure('s.f2()') IS NOT NULL}),
	"f\nt\nt",
	'a reset to the commit reads its record, and makes the routine');

$node->stop;
done_testing();
