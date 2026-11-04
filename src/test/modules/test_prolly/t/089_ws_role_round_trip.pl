# Copyright (c) 2026, PostgreSQL Global Development Group

# Group roles travel between clusters and the rest of a role stays where it is:
# what arrives is made exactly, nothing is dropped, and a role that is here for
# some other reason is not taken over.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $src = PostgreSQL::Test::Cluster->new('ws_role_src');
$src->init(extra => ['--encoding=UTF8', '--locale=C']);
$src->start;
my $dst = PostgreSQL::Test::Cluster->new('ws_role_dst');
$dst->init(extra => ['--encoding=UTF8', '--locale=C']);
$dst->start;
$_->safe_psql('postgres', 'CREATE EXTENSION test_prolly') for ($src, $dst);

my $bytes = sub {
	my ($node, @names) = @_;
	my $list = join(',', map { "'$_'" } @names);
	return $node->safe_psql('postgres', "SELECT test_ws_role_bytes(ARRAY[$list]::text[])");
};
my $restore = sub {
	my ($node, $bytes, $known) = @_;
	return $node->psql('postgres', "SELECT test_ws_role_restore('$bytes', '" . ($known // '') . "')");
};
my $roles_sql = q{SELECT string_agg(format('%s %s %s', rolname, rolcanlogin, rolinherit), E'\n' ORDER BY rolname)
	FROM pg_roles WHERE rolname LIKE 'g\_%' OR rolname LIKE 'x\_%'};
my $members_sql = q{SELECT string_agg(format('%s in %s %s %s %s', r.rolname, p.rolname, m.admin_option, m.inherit_option, m.set_option), E'\n' ORDER BY 1)
	FROM pg_auth_members m JOIN pg_roles p ON p.oid = m.roleid JOIN pg_roles r ON r.oid = m.member
	WHERE r.rolname LIKE 'g\_%' OR r.rolname LIKE 'x\_%'};

$src->safe_psql('postgres', q{
	CREATE ROLE g_top NOLOGIN;
	CREATE ROLE g_mid NOLOGIN NOINHERIT;
	CREATE ROLE g_leaf NOLOGIN;
	CREATE ROLE "g_q""uote; --" NOLOGIN;
	GRANT g_top TO g_mid WITH ADMIN TRUE, INHERIT FALSE, SET TRUE;
	GRANT g_mid TO g_leaf WITH ADMIN FALSE, INHERIT TRUE, SET FALSE;
	GRANT "g_q""uote; --" TO g_leaf;
	CREATE ROLE x_person LOGIN PASSWORD 'hunter2' VALID UNTIL '2100-01-01' CONNECTION LIMIT 3;
	ALTER ROLE g_top SET work_mem = '7MB';
	GRANT g_leaf TO x_person;
});

# what the one role names brings all it is a member of, and only those
my $held = $bytes->($src, 'g_leaf');
is(scalar(() = $held =~ /\n/g), 4, 'a role brings those it is a member of');
unlike($held, qr/\Q${\ unpack('H*', 'x_person')}\E/, 'a member that can log in stays home');

# nothing else of a role is in its bytes
for my $secret ('hunter2', 'SCRAM', '7MB', '2100')
{
	unlike($held, qr/\Q${\ unpack('H*', $secret)}\E/, "$secret stays home");
}

# the same roles, the same bytes
is($bytes->($src, 'g_mid', 'g_leaf'), $held, 'one set of roles is one set of bytes');

# a role that cannot travel is refused, by name or by being a member of it
$src->safe_psql('postgres', q{
	CREATE ROLE x_super NOLOGIN SUPERUSER;
	CREATE ROLE x_cr NOLOGIN CREATEROLE;
	CREATE ROLE x_cd NOLOGIN CREATEDB;
	CREATE ROLE x_rep NOLOGIN REPLICATION;
	CREATE ROLE x_by NOLOGIN BYPASSRLS;
	CREATE ROLE x_in_pg NOLOGIN IN ROLE pg_monitor;
	CREATE ROLE x_in_login NOLOGIN IN ROLE x_person;
	CREATE ROLE x_in_super NOLOGIN IN ROLE x_super;
});
for my $case (
	[ 'x_person', 'can log in', 'a role that can log in' ],
	[ 'x_super', 'power', 'a superuser' ],
	[ 'x_cr', 'power', 'one that can create roles' ],
	[ 'x_cd', 'power', 'one that can create databases' ],
	[ 'x_rep', 'power', 'one for replication' ],
	[ 'x_by', 'power', 'one that bypasses row security' ],
	[ 'x_in_pg', 'predefined', 'a member of a predefined role' ],
	[ 'x_in_login', 'can log in', 'a member of one that can log in' ],
	[ 'x_in_super', 'power', 'a member of a superuser' ],
	[ 'pg_monitor', 'predefined', 'a predefined role' ])
{
	my ($name, $want, $what) = @$case;
	my ($rc, $out, $err) = $src->psql('postgres', "SELECT test_ws_role_bytes(ARRAY['$name'])");
	isnt($rc, 0, "$what is refused");
	like($err, qr/cannot travel with a commit: .*$want/, "$what is refused for why");
}

my ($rc, $out, $err) = $src->psql('postgres', "SELECT test_ws_role_bytes(ARRAY['x_none'])");
like($err, qr/role "x_none" does not exist/, 'a role not there is an error');

# a membership of two grantors has options a commit could not hold
$src->safe_psql('postgres', q{
	CREATE ROLE g_two NOLOGIN;
	CREATE ROLE g_two_of NOLOGIN;
	CREATE ROLE x_admin NOLOGIN;
	GRANT g_two_of TO x_admin WITH ADMIN TRUE;
	GRANT g_two_of TO g_two WITH INHERIT FALSE;
	GRANT g_two_of TO g_two WITH INHERIT TRUE GRANTED BY x_admin;
});
($rc, $out, $err) = $src->psql('postgres', "SELECT test_ws_role_bytes(ARRAY['g_two'])");
like($err, qr/granted by more than one role/, 'a membership of two grantors is refused');

# one snapshot: a role that can log in by then is held as it was or refused, and a
# membership never sees a role otherwise than the read that found it did
$src->safe_psql('postgres', q{
	CREATE ROLE g_race NOLOGIN;
	CREATE ROLE g_race_of NOLOGIN;
	GRANT g_race_of TO g_race;
});
my $s = $src->background_psql('postgres');
$s->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$s->query_safe('SELECT count(*) FROM pg_roles');
$src->safe_psql('postgres', 'ALTER ROLE g_race_of LOGIN');
my $old = $s->query_safe(q{SELECT test_ws_role_bytes(ARRAY['g_race'])});
$s->query_safe('COMMIT');
like($old, qr/\Q${\ unpack('H*', 'g_race_of')}\E/, 'a read is of its snapshot alone');
($rc, $out, $err) = $src->psql('postgres', "SELECT test_ws_role_bytes(ARRAY['g_race'])");
like($err, qr/role "g_race_of" cannot travel with a commit: it can log in/, 'and the next is of its own');
$s->quit;

# arriving, exactly
is($restore->($dst, $held), 0, 'roles arrive on a cluster without them');
is($dst->safe_psql('postgres', $roles_sql), q{g_leaf f t
g_mid f f
g_q"uote; -- f t
g_top f t}, 'as they were, none able to log in');
is($dst->safe_psql('postgres', $members_sql), q{g_leaf in g_mid f t f
g_leaf in g_q"uote; -- f t t
g_mid in g_top t f t}, 'with their memberships as they were');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_authid WHERE rolname LIKE 'g\_%' AND rolpassword IS NOT NULL}),
	'0', 'with no password');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_db_role_setting s JOIN pg_roles r ON r.oid = s.setrole WHERE r.rolname = 'g_top'}),
	'0', 'and no settings');
is($bytes->($dst, 'g_leaf'), $held, 'and the same bytes there');

# arriving again changes nothing, a role not here having been made and none here having been dropped
my $auth_before = $dst->safe_psql('postgres', 'SELECT string_agg(oid::text, $$ $$ ORDER BY oid) FROM pg_auth_members');
is($restore->($dst, $held, $held), 0, 'arriving again is allowed, being known');
is($dst->safe_psql('postgres', 'SELECT string_agg(oid::text, $$ $$ ORDER BY oid) FROM pg_auth_members'),
	$auth_before, 'and grants nothing again');

# a membership of a local role stays, a commit having no say in it
$dst->safe_psql('postgres', q{CREATE ROLE x_here LOGIN; GRANT g_leaf TO x_here;
	CREATE ROLE g_extra NOLOGIN; GRANT g_extra TO g_top;});
is($restore->($dst, $held, $held), 0, 'it arrives with others here');
is($dst->safe_psql('postgres', q{SELECT string_agg(r.rolname, ' ' ORDER BY r.rolname) FROM pg_auth_members m
	JOIN pg_roles p ON p.oid = m.roleid JOIN pg_roles r ON r.oid = m.member WHERE p.rolname IN ('g_leaf', 'g_extra')}),
	'g_top x_here', 'and nothing is dropped');

# a changed role changes here; a changed membership's options are the commit's
$src->safe_psql('postgres', 'ALTER ROLE g_top NOINHERIT; ALTER ROLE g_mid INHERIT');
my $changed = $bytes->($src, 'g_leaf');
is($restore->($dst, $changed, $held), 0, 'a changed role arrives');
is($dst->safe_psql('postgres', q{SELECT string_agg(rolname || ' ' || rolinherit, ', ' ORDER BY rolname) FROM pg_roles WHERE rolname IN ('g_top', 'g_mid')}),
	'g_mid true, g_top false', 'as it is now');

# a role here that this database does not know is not taken over: its members would get the commit's grants
$dst->safe_psql('postgres', q{CREATE ROLE g_new NOLOGIN; CREATE ROLE x_friend LOGIN; GRANT g_new TO x_friend});
$src->safe_psql('postgres', 'CREATE ROLE g_new NOLOGIN');
my $with_new = $bytes->($src, 'g_leaf', 'g_new');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$with_new', '$held')");
like($err, qr/role "g_new" is here already, and not from this database/, 'a role here by another way refuses arrival');
is($dst->safe_psql('postgres', q{SELECT rolinherit FROM pg_roles WHERE rolname = 'g_top'}), 'f', 'making nothing');

# nor one here that could not travel, known or not
$dst->safe_psql('postgres', 'ALTER ROLE g_top LOGIN');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$changed', '$changed')");
like($err, qr/role "g_top" cannot arrive here: it can log in/, 'a known role that can log in here refuses arrival');
$dst->safe_psql('postgres', 'ALTER ROLE g_top NOLOGIN SUPERUSER');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$changed', '$changed')");
like($err, qr/role "g_top" cannot arrive here: .*power/, 'a known role with a power here refuses arrival');
$dst->safe_psql('postgres', 'ALTER ROLE g_top NOSUPERUSER');

# a commit holding a membership of a role it does not hold: nothing here could say what that role is
my ($first) = split /\n/, $changed;
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$first\n', '$changed')");
like($err, qr/is a member of .* which it does not hold/, 'a dangling membership refused');

# a role's name in the commit reaches only a role of that name
my $evil = sprintf("%s:%s\n", unpack('H*', 'g_evil'), '00' . '01' . '06' . unpack('H*', 'x_here') . '01');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$evil', '')");
like($err, qr/does not hold/, 'a commit cannot make its role a member of a local one');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_roles WHERE rolname = 'g_evil'}), '0', 'nor make it');

$_->stop for ($src, $dst);
done_testing();
