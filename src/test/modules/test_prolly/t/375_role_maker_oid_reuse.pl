# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch, tag or stash maker is a shared dependency of its role, so no
# record of a role's OID outlives the role: DROP ROLE refuses a role that
# still makes any, and once DROP OWNED has given its branch to the
# database's owner and deleted its tag and stash, a new role given the old
# role's OID again (here by pg_resetwal's next OID) owns none of them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('role_maker_oid_reuse');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA r VERSIONED;
	CREATE TABLE r.t (a int PRIMARY KEY);
	GRANT USAGE ON SCHEMA r TO PUBLIC;
	GRANT ALL ON r.t TO PUBLIC;
	INSERT INTO r.t VALUES (1);
	SELECT dolt_commit('-Am', 'one');
	CREATE ROLE regress_gone;
	GRANT CREATE ON DATABASE postgres TO regress_gone});
sql(q{SET ROLE regress_gone;
	SELECT dolt_branch('feat');
	SELECT dolt_tag('v1');
	INSERT INTO r.t VALUES (2);
	SELECT dolt_stash('push', 'shelf')});
my $oid = sql(q{SELECT 'regress_gone'::regrole::oid});
is(sql(q{SELECT creator FROM dolt_branches WHERE name = 'feat'}),
	'regress_gone', 'the branch is its maker\'s');

my ($ret, $stdout, $stderr) = $node->psql('postgres',
	q{REVOKE CREATE ON DATABASE postgres FROM regress_gone;
	DROP ROLE regress_gone});
isnt($ret, 0, 'DROP ROLE refuses a role that makes a branch');
like($stderr, qr/owner of branch "feat"/, 'and names the branch');
like($stderr, qr/owner of tag "v1"/, 'the tag');
like($stderr, qr/owner of stash stash\@\{0\} of "shelf"/, 'and the stash');

sql(q{DROP OWNED BY regress_gone; DROP ROLE regress_gone});
is(sql(q{SELECT count(*) FROM pg_shdepend WHERE refobjid = } . $oid),
	'0', 'no entry names the dropped role');

# a role made gets the dropped role's OID: the dropped role's row is
# vacuumed away (a new OID skips one a dead row still has) and the OID
# counter set to it
sql(q{VACUUM pg_authid});
$node->stop;
command_ok([ 'pg_resetwal', '-o', $oid, $node->data_dir ],
	'next OID set to the dropped role\'s');
$node->start;
sql(q{CREATE ROLE regress_heir});
is(sql(q{SELECT 'regress_heir'::regrole::oid}), $oid,
	'the new role has the dropped role\'s OID');

is(sql(q{SELECT creator = current_user FROM dolt_branches WHERE name = 'feat'}),
	't', 'the branch is the database owner\'s, not the new role\'s');
($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SET ROLE regress_heir; SELECT dolt_branch('-D', 'feat')});
like($stderr, qr/permission denied to move the head of branch "feat"/,
	'the new role can\'t delete the branch');
is(sql(q{SELECT count(*) FROM dolt_tags}), '0', 'the tag was deleted');
is(sql(q{SELECT count(*) FROM dolt_stashes}), '0', 'and the stash');

# A role that makes a branch in another database: DROP ROLE's hint names
# DROP OWNED there, which lets it go.
$node->safe_psql('postgres', q{CREATE DATABASE far;
	CREATE ROLE regress_far;
	GRANT CREATE ON DATABASE far TO regress_far});
$node->safe_psql('far', q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA r VERSIONED;
	CREATE TABLE r.t (a int PRIMARY KEY);
	INSERT INTO r.t VALUES (1);
	SELECT dolt_commit('-Am', 'one');
	SET ROLE regress_far;
	SELECT dolt_branch('far_branch')});
($ret, $stdout, $stderr) = $node->psql('postgres',
	q{REVOKE CREATE ON DATABASE far FROM regress_far; DROP ROLE regress_far});
isnt($ret, 0, 'DROP ROLE refuses a role that makes a branch in another database');
like($stderr, qr/REASSIGN OWNED or DROP OWNED there changes the branches/,
	'and its hint names DROP OWNED there');
$node->safe_psql('far', q{DROP OWNED BY regress_far});
$node->safe_psql('postgres', q{DROP ROLE regress_far});
pass('DROP OWNED there lets the role go');

$node->stop;
done_testing();
