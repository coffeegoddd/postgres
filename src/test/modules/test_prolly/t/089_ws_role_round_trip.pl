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
	ALTER ROLE g_top PASSWORD 'hunter2' VALID UNTIL '2100-01-01' CONNECTION LIMIT 3;
	GRANT g_leaf TO x_person;
});

# what the one role names brings all it is a member of, and only those
my $held = $bytes->($src, 'g_leaf');
is(scalar(() = $held =~ /\n/g), 4, 'a role brings those it is a member of');
unlike($held, qr/\Q${\ unpack('H*', 'x_person')}\E/, 'a member that can log in stays home');

# nothing else of a role is in its bytes: all there is of one with every secret is
# its name, no flags and no memberships
my ($top_bytes) = grep { /^\Q${\ unpack('H*', 'g_top')}\E:/ } split /\n/, $held;
is($top_bytes, unpack('H*', 'g_top') . ':0000', 'a password, expiry, limit and setting stay home');

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

# capture marks what it holds, which takes a superuser
$src->safe_psql('postgres', q{CREATE ROLE x_nobody LOGIN NOSUPERUSER; CREATE ROLE g_notmine NOLOGIN;});
my $notmine = $bytes->($src, 'g_notmine');
($rc, $out, $err) = $src->psql('postgres', "SET ROLE x_nobody; SELECT test_ws_role_mark('$notmine')");
like($err, qr/permission denied to let role "g_notmine" travel/, 'marking a role takes a superuser');
$src->safe_psql('postgres', q{GRANT g_notmine TO x_nobody WITH ADMIN TRUE});
($rc, $out, $err) = $src->psql('postgres', "SET ROLE x_nobody; SELECT test_ws_role_mark('$notmine')");
like($err, qr/permission denied to let role "g_notmine" travel/, 'ADMIN on it is not enough');
is($src->safe_psql('postgres', q{SELECT roltravels FROM pg_authid WHERE rolname = 'g_notmine'}), 'f', 'leaving it unmarked');
$src->safe_psql('postgres', "SELECT test_ws_role_mark('$held')");
is($src->safe_psql('postgres', q{SELECT count(*) FROM pg_authid WHERE rolname LIKE 'g\_%' AND roltravels}), '4', 'capture marks those it holds');

# arriving, exactly
is($restore->($dst, $held), 0, 'roles arrive on a cluster without them');
is($dst->safe_psql('postgres', $roles_sql), q{g_leaf f t
g_mid f f
g_q"uote; -- f t
g_top f t}, 'as they were, none able to log in');
is($dst->safe_psql('postgres', $members_sql), q{g_leaf in g_mid f t f
g_leaf in g_q"uote; -- f t t
g_mid in g_top t f t}, 'with their memberships as they were');
is( $dst->safe_psql('postgres', q{SELECT rolconnlimit, rolvaliduntil IS NULL, rolpassword IS NULL FROM pg_authid WHERE rolname = 'g_top'}),
	'-1|t|t', 'with no limit, expiry or password');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_db_role_setting s JOIN pg_roles r ON r.oid = s.setrole WHERE r.rolname = 'g_top'}),
	'0', 'and no settings');
is($bytes->($dst, 'g_leaf'), $held, 'and the same bytes there');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_authid WHERE rolname LIKE 'g\_%' AND roltravels}), '4', 'and marked');

# arriving again changes nothing
my $auth_before = $dst->safe_psql('postgres', 'SELECT string_agg(oid::text, $$ $$ ORDER BY oid) FROM pg_auth_members');
is($restore->($dst, $held, $held), 0, 'arriving again is allowed');
is($dst->safe_psql('postgres', 'SELECT string_agg(oid::text, $$ $$ ORDER BY oid) FROM pg_auth_members'),
	$auth_before, 'and grants nothing again');

# leaving a root and coming back, and a second clone on one cluster
is($restore->($dst, $bytes->($src, 'g_top'), $held), 0, 'a commit may hold fewer');
is($restore->($dst, $held, $bytes->($src, 'g_top')), 0, 'and a role that left may come back');
$dst->safe_psql('postgres', 'CREATE DATABASE other');
$dst->safe_psql('other', 'CREATE EXTENSION test_prolly');
is($dst->psql('other', "SELECT test_ws_role_restore('$held', '')"), 0, 'and a second database on this cluster may hold them');

# what is made here stays; nothing is dropped
$dst->safe_psql('postgres', q{CREATE ROLE x_here LOGIN; GRANT g_leaf TO x_here;
	GRANT g_top TO g_leaf});
is($restore->($dst, $held, $held), 0, 'it arrives with memberships made here');
is($dst->safe_psql('postgres', q{SELECT string_agg(r.rolname || ' in ' || p.rolname, ', ' ORDER BY r.rolname) FROM pg_auth_members m
	JOIN pg_roles p ON p.oid = m.roleid JOIN pg_roles r ON r.oid = m.member
	WHERE (r.rolname, p.rolname) IN (('x_here', 'g_leaf'), ('g_leaf', 'g_top'))}),
	'g_leaf in g_top, x_here in g_leaf', 'and nothing made here is dropped, whether between roles it holds or not');
$dst->safe_psql('postgres', 'REVOKE g_top FROM g_leaf');

# a changed role changes here
$src->safe_psql('postgres', 'ALTER ROLE g_top NOINHERIT; ALTER ROLE g_mid INHERIT');
my $changed = $bytes->($src, 'g_leaf');
is($restore->($dst, $changed, $held), 0, 'a changed role arrives');
is($dst->safe_psql('postgres', q{SELECT string_agg(rolname || ' ' || rolinherit, ', ' ORDER BY rolname) FROM pg_roles WHERE rolname IN ('g_top', 'g_mid')}),
	'g_mid true, g_top false', 'as it is now');
is($bytes->($dst, 'g_leaf'), $changed, 'with the same bytes');

# a membership's options, whoever granted what is here: one lowered is lower
$dst->safe_psql('postgres', q{CREATE ROLE x_cr LOGIN CREATEROLE; GRANT g_top TO x_cr WITH ADMIN TRUE; GRANT g_mid TO x_cr WITH ADMIN TRUE;
	SET ROLE x_cr; GRANT g_top TO g_mid WITH ADMIN TRUE, INHERIT FALSE, SET TRUE});
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_auth_members m JOIN pg_roles p ON p.oid = m.roleid
	JOIN pg_roles r ON r.oid = m.member WHERE p.rolname = 'g_top' AND r.rolname = 'g_mid'}), '2', 'a second grantor here gives two rows');
$src->safe_psql('postgres', 'REVOKE g_top FROM g_mid; GRANT g_top TO g_mid WITH ADMIN FALSE, INHERIT FALSE, SET TRUE');
my $lowered = $bytes->($src, 'g_leaf');
is($restore->($dst, $lowered, $changed), 0, 'a lowered membership arrives');
is($dst->safe_psql('postgres', q{SELECT count(*), bool_or(m.admin_option) FROM pg_auth_members m JOIN pg_roles p ON p.oid = m.roleid
	JOIN pg_roles r ON r.oid = m.member WHERE p.rolname = 'g_top' AND r.rolname = 'g_mid'}), '1|f', 'and no row of another grantor keeps what it took');
is($bytes->($dst, 'g_leaf'), $lowered, 'so the same bytes come back');

# a membership the commit removed is removed, so the next commit does not put it back
$src->safe_psql('postgres', 'REVOKE "g_q""uote; --" FROM g_leaf');
my $revoked = $bytes->($src, 'g_leaf', 'g_q"uote; --');
is($restore->($dst, $revoked, $lowered), 0, 'a commit may remove a membership');
is($bytes->($dst, 'g_leaf', 'g_q"uote; --'), $revoked, 'and it goes');

# one reversed in a single commit
$src->safe_psql('postgres', q{REVOKE g_mid FROM g_leaf; GRANT g_leaf TO g_mid});
my $reversed = $bytes->($src, 'g_leaf', 'g_mid');
is($restore->($dst, $reversed, $revoked), 0, 'a commit may reverse a membership');
is($bytes->($dst, 'g_leaf', 'g_mid'), $reversed, 'whichever way it went');

# a cycle in a commit makes nothing
my $cycle = sprintf("%s:0001%02x%s01\n%s:0001%02x%s01\n",
	unpack('H*', 'g_c1'), 4, unpack('H*', 'g_c2'), unpack('H*', 'g_c2'), 4, unpack('H*', 'g_c1'));
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$cycle', '')");
like($err, qr/is a member of role/, 'a cycle of memberships is refused');

# a commit reaches no role here that it does not hold, through memberships made here
$src->safe_psql('postgres', q{CREATE ROLE g_low NOLOGIN; CREATE ROLE g_ops NOLOGIN});
my $apart = $bytes->($src, 'g_low', 'g_ops');
is($restore->($dst, $apart), 0, 'two roles arrive apart');
$dst->safe_psql('postgres', q{CREATE ROLE x_su NOLOGIN SUPERUSER; GRANT x_su TO g_ops;
	CREATE ROLE x_intern LOGIN; GRANT g_low TO x_intern});
$src->safe_psql('postgres', q{GRANT g_ops TO g_low});
my $reaching = $bytes->($src, 'g_low', 'g_ops');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$reaching', '$apart')");
like($err, qr/role "g_low" would reach role "x_su", which it does not hold/, 'a superuser cannot be reached through a membership made here');
$dst->safe_psql('postgres', q{REVOKE x_su FROM g_ops; GRANT pg_read_server_files TO g_ops});
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$reaching', '$apart')");
like($err, qr/would reach role "pg_read_server_files"/, 'nor a predefined role');
$dst->safe_psql('postgres', q{REVOKE pg_read_server_files FROM g_ops; CREATE ROLE x_two NOLOGIN; CREATE ROLE x_three LOGIN;
	GRANT x_two TO g_ops; GRANT x_three TO x_two});
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$reaching', '$apart')");
like($err, qr/would reach role "x_two"/, 'nor a group role made here');
$dst->safe_psql('postgres', q{REVOKE x_two FROM g_ops; CREATE ROLE g_far NOLOGIN TRAVELS; GRANT g_far TO g_ops; GRANT x_su TO g_far});
$src->safe_psql('postgres', q{CREATE ROLE g_far NOLOGIN});
my $far = $bytes->($src, 'g_low', 'g_ops', 'g_far');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$far', '$apart')");
like($err, qr/would reach role "x_su"/, 'nor anything further, through a role the commit holds');
$dst->safe_psql('postgres', q{REVOKE x_su FROM g_far; REVOKE g_far FROM g_ops});
is($restore->($dst, $reaching, $apart), 0, 'with those gone it arrives');

# a role here by another way is not taken over: its members would get the commit's grants
$dst->safe_psql('postgres', q{CREATE ROLE g_new NOLOGIN; CREATE ROLE x_friend LOGIN; GRANT g_new TO x_friend});
$src->safe_psql('postgres', 'CREATE ROLE g_new NOLOGIN; CREATE ROLE g_fresh NOLOGIN');
my $with_new = $bytes->($src, 'g_fresh', 'g_new');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$with_new', '')");
like($err, qr/role "g_new" is here already, and not held by any commit here/, 'a role here by another way refuses arrival');

# nor one here that could not travel, marked or not
$dst->safe_psql('postgres', 'ALTER ROLE g_top LOGIN');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$held', '$held')");
like($err, qr/role "g_top" cannot arrive here: it can log in/, 'a marked role that can log in here refuses arrival');
$dst->safe_psql('postgres', 'ALTER ROLE g_top NOLOGIN SUPERUSER');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$held', '$held')");
like($err, qr/role "g_top" cannot arrive here: .*power/, 'a marked role with a power here refuses arrival');
$dst->safe_psql('postgres', 'ALTER ROLE g_top NOSUPERUSER');

# a role's mark outlives a dump of globals, and only a superuser sets it by SQL
$src->safe_psql('postgres', q{CREATE ROLE g_dumped NOLOGIN TRAVELS; CREATE ROLE g_undumped NOLOGIN});
is($src->safe_psql('postgres', q{SELECT rolname, roltravels FROM pg_roles WHERE rolname = 'g_dumped'}), 'g_dumped|t', 'a role may be made marked, and shown');
my $globals_file = $src->basedir . '/globals.sql';
$src->command_ok([ 'pg_dumpall', '--globals-only', '--port' => $src->port, '--host' => $src->host, '--file' => $globals_file ], 'pg_dumpall of globals');
my $globals = PostgreSQL::Test::Utils::slurp_file($globals_file);
like($globals, qr/ALTER ROLE g_dumped WITH [^\n]* TRAVELS;/, 'a mark is dumped');
like($globals, qr/ALTER ROLE g_undumped WITH [^\n]* NOTRAVELS;/, 'and its absence');
my $third = PostgreSQL::Test::Cluster->new('ws_role_third');
$third->init(extra => ['--encoding=UTF8', '--locale=C']);
$third->start;
$third->safe_psql('postgres', 'CREATE ROLE g_undumped NOLOGIN TRAVELS');
$third->psql('postgres', join("\n", grep { /^(CREATE|ALTER) ROLE g_(un)?dumped/ } split /\n/, $globals), on_error_stop => 0);
is( $third->safe_psql('postgres', q{SELECT string_agg(rolname || ' ' || roltravels, ', ' ORDER BY rolname) FROM pg_roles WHERE rolname IN ('g_dumped', 'g_undumped')}),
	'g_dumped true, g_undumped false', 'and a restore leaves both as they were, over a role there too');
$third->stop;
$dst->safe_psql('postgres', 'CREATE ROLE g_marked_by_sql NOLOGIN; ALTER ROLE g_marked_by_sql TRAVELS');
is($dst->safe_psql('postgres', q{SELECT roltravels FROM pg_authid WHERE rolname = 'g_marked_by_sql'}), 't', 'ALTER ROLE marks');
$dst->safe_psql('postgres', 'ALTER ROLE g_marked_by_sql NOTRAVELS');
is($dst->safe_psql('postgres', q{SELECT roltravels FROM pg_authid WHERE rolname = 'g_marked_by_sql'}), 'f', 'and clears');
$dst->safe_psql('postgres', q{CREATE ROLE x_plain LOGIN; CREATE ROLE x_admin_cr LOGIN CREATEROLE; GRANT g_marked_by_sql TO x_admin_cr WITH ADMIN TRUE;});
($rc, $out, $err) = $dst->psql('postgres', q{SET ROLE x_plain; ALTER ROLE g_marked_by_sql TRAVELS});
like($err, qr/permission denied to alter role/, 'which a role with no rights on it cannot');
($rc, $out, $err) = $dst->psql('postgres', q{SET ROLE x_admin_cr; ALTER ROLE g_marked_by_sql TRAVELS});
like($err, qr/Only roles with the SUPERUSER attribute may change the TRAVELS attribute/, 'nor one with CREATEROLE and ADMIN');
($rc, $out, $err) = $dst->psql('postgres', q{SET ROLE x_admin_cr; CREATE ROLE g_squat NOLOGIN TRAVELS});
like($err, qr/may specify the TRAVELS attribute/, 'nor may one make a role marked');

# one that cannot travel cannot be marked, by name
my $forged_login = sprintf("%s:0000\n", unpack('H*', 'x_person'));
($rc, $out, $err) = $src->psql('postgres', "SELECT test_ws_role_mark('$forged_login')");
like($err, qr/role "x_person" cannot travel with a commit: it can log in/, 'a login role cannot be marked');

# what has nothing to do takes no lock: an open change of owner does not block it
$src->safe_psql('postgres', 'CREATE ROLE g_race_mark NOLOGIN TRAVELS');
my $to_mark = $bytes->($src, 'g_race_mark');
my $m1 = $src->background_psql('postgres');
$m1->query_safe('BEGIN');
$m1->query_safe(q{CREATE TABLE owned (); ALTER TABLE owned OWNER TO g_race_mark});
is($src->psql('postgres', "SET lock_timeout = '5s'; SELECT test_ws_role_mark('$to_mark')"), 0, 'marking one already marked waits for no one');
$m1->query_safe('ROLLBACK');
$m1->quit;

# two markings of one unmarked role: the second waits, then has nothing to do
$src->safe_psql('postgres', 'CREATE ROLE g_race_mark2 NOLOGIN');
my $to_mark2 = $bytes->($src, 'g_race_mark2');
my $m3 = $src->background_psql('postgres');
$m3->query_safe('BEGIN');
$m3->query_safe("SELECT test_ws_role_mark('$to_mark2')");
my $m4 = $src->background_psql('postgres', on_error_stop => 0);
$m4->query_until(qr/started/, "\\echo started\nSELECT test_ws_role_mark('$to_mark2');\n");
$src->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted AND locktype = 'object'}) or die 'it did not wait';
$m3->query_safe('COMMIT');
like($m4->query('SELECT 1 AS after'), qr/1/, 'two markings of one role do not collide');
unlike($m4->{stderr}, qr/ERROR/, 'with no error');
$m4->quit;
$m3->quit;

# a commit's membership of a role it does not hold: it cannot join one here
my $evil = sprintf("%s:0001%02x%s01\n", unpack('H*', 'g_evil'), 6, unpack('H*', 'x_here'));
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$evil', '')");
like($err, qr/is a member of "x_here", which it does not hold/, 'a commit cannot join a role here');

# a name that reads as a keyword is that name
$src->safe_psql('postgres', q{CREATE ROLE "select" NOLOGIN; CREATE ROLE "current_user" NOLOGIN; GRANT "select" TO "current_user" WITH ADMIN FALSE, INHERIT TRUE, SET TRUE});
my $words = $bytes->($src, 'current_user');
is($restore->($dst, $words), 0, 'a role named as a keyword arrives');
is($bytes->($dst, 'current_user'), $words, 'as that role');

# restoring with CREATEROLE alone
$dst->safe_psql('postgres', q{CREATE ROLE x_maker LOGIN CREATEROLE});
$src->safe_psql('postgres', q{CREATE ROLE g_cr1 NOLOGIN; CREATE ROLE g_cr2 NOLOGIN IN ROLE g_cr1});
my $cr = $bytes->($src, 'g_cr2');
($rc, $out, $err) = $dst->psql('postgres', "SET ROLE x_maker; SELECT test_ws_role_restore('$cr', '')");
is($rc, 0, 'CREATEROLE can make new roles from a commit');
is($bytes->($dst, 'g_cr2'), $cr, 'as they are held');
($rc, $out, $err) = $dst->psql('postgres', "SET ROLE x_maker; SELECT test_ws_role_restore('$changed', '$changed')");
like($err, qr/permission denied/, 'but not change those it has no ADMIN on');

# a commit that only lowers is no reach, and takes no more than the membership
$src->safe_psql('postgres', q{CREATE ROLE g_l1 NOLOGIN; CREATE ROLE g_l2 NOLOGIN;
	GRANT g_l2 TO g_l1 WITH ADMIN TRUE, INHERIT TRUE, SET TRUE});
my $l_high = $bytes->($src, 'g_l1');
is($restore->($dst, $l_high), 0, 'a membership arrives');
$dst->safe_psql('postgres', q{CREATE ROLE x_local_l NOLOGIN; GRANT x_local_l TO g_l2});
$src->safe_psql('postgres', q{GRANT g_l2 TO g_l1 WITH ADMIN FALSE, INHERIT TRUE, SET TRUE});
my $l_low = $bytes->($src, 'g_l1');
is($restore->($dst, $l_low, $l_high), 0, 'a commit taking access away is not refused for what is local');
is($bytes->($dst, 'g_l1') =~ s/\n.*$//sr, $l_low =~ s/\n.*$//sr, 'and lowers it');
$dst->safe_psql('postgres', 'REVOKE x_local_l FROM g_l2');

# through a membership the same commit removes: it is gone before anything is added
$src->safe_psql('postgres', q{CREATE ROLE g_m1 NOLOGIN; CREATE ROLE g_m2 NOLOGIN; CREATE ROLE g_m3 NOLOGIN; GRANT g_m3 TO g_m2});
my $m_before = $bytes->($src, 'g_m1', 'g_m2', 'g_m3');
is($restore->($dst, $m_before), 0, 'memberships arrive');
$dst->safe_psql('postgres', q{GRANT x_su TO g_m3});
$src->safe_psql('postgres', q{REVOKE g_m3 FROM g_m2; GRANT g_m2 TO g_m1});
my $m_after = $bytes->($src, 'g_m1', 'g_m2', 'g_m3');
is($restore->($dst, $m_after, $m_before), 0, 'a membership removed by the commit reaches nothing');
$dst->safe_psql('postgres', q{REVOKE x_su FROM g_m3});

# a role that leaves a commit takes its memberships with it
$src->safe_psql('postgres', q{CREATE ROLE g_r1 NOLOGIN; CREATE ROLE g_r2 NOLOGIN; GRANT g_r2 TO g_r1});
my $r_both = $bytes->($src, 'g_r1');
is($restore->($dst, $r_both), 0, 'a membership arrives');
$src->safe_psql('postgres', q{REVOKE g_r2 FROM g_r1});
my $r_one = $bytes->($src, 'g_r1');
is($restore->($dst, $r_one, $r_both), 0, 'a commit retires a role');
is($bytes->($dst, 'g_r1'), $r_one, 'and the member is no longer a member of it');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_roles WHERE rolname = 'g_r2'}), '1', 'and it stays here, not dropped');
$src->safe_psql('postgres', q{GRANT g_r2 TO g_r1});
$r_both = $bytes->($src, 'g_r1');
is($restore->($dst, $r_both, $r_one), 0, 'a membership comes back');
my $r_target = $bytes->($src, 'g_r2');
is($restore->($dst, $r_target, $r_both), 0, 'a commit retires the member instead');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_auth_members m JOIN pg_roles r ON r.oid = m.member WHERE r.rolname = 'g_r1'}), '0',
	'so its members here get nothing from it');

# what depends on a membership here: an option changes in place; ADMIN is refused by name
$src->safe_psql('postgres', q{CREATE ROLE g_d1 NOLOGIN; CREATE ROLE g_d2 NOLOGIN; GRANT g_d2 TO g_d1 WITH ADMIN TRUE, INHERIT TRUE, SET TRUE});
my $d_before = $bytes->($src, 'g_d1');
is($restore->($dst, $d_before), 0, 'a membership with ADMIN arrives');
$dst->safe_psql('postgres', q{CREATE ROLE x_w LOGIN; GRANT g_d2 TO x_w GRANTED BY g_d1});
$src->safe_psql('postgres', q{GRANT g_d2 TO g_d1 WITH ADMIN TRUE, INHERIT FALSE, SET TRUE});
my $d_inherit = $bytes->($src, 'g_d1');
is($restore->($dst, $d_inherit, $d_before), 0, 'an option changes under a grant that depends on it');
is($bytes->($dst, 'g_d1'), $d_inherit, 'and is changed');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_auth_members m JOIN pg_roles r ON r.oid = m.member WHERE r.rolname = 'x_w'}), '1', 'and the grant stays');
$src->safe_psql('postgres', q{GRANT g_d2 TO g_d1 WITH ADMIN FALSE, INHERIT FALSE, SET TRUE});
my $d_low = $bytes->($src, 'g_d1');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$d_low', '$d_inherit')");
like($err, qr/takes the ADMIN option on role "g_d2" from role "g_d1", which granted it here to role "x_w"/, 'lowering ADMIN that was used is refused by name');
$src->safe_psql('postgres', q{REVOKE g_d2 FROM g_d1});
my $d_gone = $bytes->($src, 'g_d1', 'g_d2');
($rc, $out, $err) = $dst->psql('postgres', "SELECT test_ws_role_restore('$d_gone', '$d_inherit')");
like($err, qr/which granted it here to role "x_w"/, 'and so is removing it');

# a grant of a superuser's that one with CREATEROLE and ADMIN has no power over
$src->safe_psql('postgres', q{CREATE ROLE g_s1 NOLOGIN; CREATE ROLE g_s2 NOLOGIN; GRANT g_s2 TO g_s1 WITH ADMIN FALSE, INHERIT TRUE, SET FALSE});
my $s_before = $bytes->($src, 'g_s1');
is($restore->($dst, $s_before), 0, 'a superuser restores a membership');
$dst->safe_psql('postgres', q{GRANT g_s1 TO x_maker WITH ADMIN TRUE; GRANT g_s2 TO x_maker WITH ADMIN TRUE});
$src->safe_psql('postgres', q{GRANT g_s2 TO g_s1 WITH ADMIN FALSE, INHERIT FALSE, SET FALSE});
my $s_after = $bytes->($src, 'g_s1');
($rc, $out, $err) = $dst->psql('postgres', "SET ROLE x_maker; SELECT test_ws_role_restore('$s_after', '$s_before')");
like($err, qr/permission denied/, 'with CREATEROLE and ADMIN another\'s grant cannot be changed');

# what a commit removes is only ever between two roles it may change
$src->safe_psql('postgres', q{CREATE ROLE g_x1 NOLOGIN; CREATE ROLE g_x2 NOLOGIN; GRANT g_x2 TO g_x1});
my $x_both = $bytes->($src, 'g_x1');
is($restore->($dst, $x_both), 0, 'a membership arrives');
$dst->safe_psql('postgres', q{REVOKE g_x2 FROM g_x1; DROP ROLE g_x1; CREATE ROLE g_x1 LOGIN; GRANT g_x2 TO g_x1});
is($restore->($dst, $bytes->($src, 'g_top'), $x_both), 0, 'a commit without them arrives');
is($dst->safe_psql('postgres', q{SELECT count(*) FROM pg_auth_members m JOIN pg_roles r ON r.oid = m.member WHERE r.rolname = 'g_x1'}), '1',
	'and leaves alone a person who took the name, and their membership');
$dst->safe_psql('postgres', q{DROP ROLE g_x1; DROP ROLE g_x2});
is($restore->($dst, $bytes->($src, 'g_top'), $x_both), 0, 'and roles dropped here are nothing to it');

# a grantor no one can act as: the row is replaced
$src->safe_psql('postgres', q{CREATE ROLE g_y1 NOLOGIN; CREATE ROLE g_y2 NOLOGIN; CREATE ROLE g_y3 NOLOGIN;
	GRANT g_y2 TO g_y1 WITH ADMIN TRUE, INHERIT TRUE, SET TRUE});
my $y_before = $bytes->($src, 'g_y1', 'g_y3');
is($restore->($dst, $y_before), 0, 'a membership arrives');
my $su = $dst->safe_psql('postgres', 'SELECT current_user');
$dst->safe_psql('postgres', qq{GRANT g_y2 TO g_y3 WITH ADMIN TRUE; SET ROLE g_y3; GRANT g_y2 TO g_y1 WITH ADMIN FALSE, INHERIT FALSE, SET TRUE; RESET ROLE;
	REVOKE g_y2 FROM g_y1 GRANTED BY "$su"; GRANT g_y2 TO g_y3 WITH ADMIN FALSE });
$src->safe_psql('postgres', q{GRANT g_y2 TO g_y1 WITH ADMIN FALSE, INHERIT TRUE, SET TRUE});
my $y_after = $bytes->($src, 'g_y1', 'g_y3');
is($restore->($dst, $y_after, $y_before), 0, 'a grantor that lost its ADMIN does not stop a commit');
is( $dst->safe_psql('postgres', q{SELECT count(*), bool_or(m.inherit_option), bool_or(m.admin_option), bool_or(g.rolname = 'g_y3') FROM pg_auth_members m
	JOIN pg_roles r ON r.oid = m.member JOIN pg_roles p ON p.oid = m.roleid JOIN pg_roles g ON g.oid = m.grantor WHERE r.rolname = 'g_y1' AND p.rolname = 'g_y2'}),
	'1|t|f|f', 'as the row is replaced');

# what one removal depends on, another may remove: names in either order
for my $names (['g_dep_a', 'g_dep_z'], ['g_dep_z2', 'g_dep_a2'])
{
	my ($giver, $taker) = @$names;
	my $top = "${giver}_top";
	$src->safe_psql('postgres', qq{CREATE ROLE $giver NOLOGIN; CREATE ROLE $taker NOLOGIN; CREATE ROLE $top NOLOGIN;
		GRANT $top TO $giver WITH ADMIN TRUE; SET ROLE $giver; GRANT $top TO $taker});
	my $with = $bytes->($src, $giver, $taker);
	is($restore->($dst, $with), 0, "memberships of $giver arrive");
	$dst->safe_psql('postgres', qq{REVOKE $top FROM $taker; GRANT $top TO $taker GRANTED BY $giver});
	$src->safe_psql('postgres', qq{REVOKE $top FROM $taker GRANTED BY $giver; REVOKE $top FROM $giver});
	my $without = $bytes->($src, $giver, $taker, $top);
	is($restore->($dst, $without, $with), 0, "and $giver with what it passed on go together");
	is($bytes->($dst, $giver, $taker, $top), $without, 'as the commit says');
}

$_->stop for ($src, $dst);
done_testing();
