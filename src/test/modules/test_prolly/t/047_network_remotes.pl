# Copyright (c) 2026, PostgreSQL Global Development Group

# Remotes over a libpq connection: another server's database cloned,
# pulled from and pushed to, its branch moved with its rows; a push over
# uncommitted changes there refused unless forced; a role that isn't a
# superuser needs a password, and the remote's pg_prolly_fetch; and a
# remote that takes only TLS connections, used over TLS.

use strict;
use warnings FATAL => 'all';
use File::Copy;
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $origin = PostgreSQL::Test::Cluster->new('net_origin');
$origin->init;
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('net_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

my $url = 'postgresql:///postgres?host=' . $origin->host . '&port=' . $origin->port;
sub o { return $origin->safe_psql($_[0], $_[1]); }
sub l { return $local->safe_psql($_[0], $_[1]); }

# Lines ahead of the origin's pg_hba.conf's, which come first.
sub prepend_hba
{
	my $hba = $origin->data_dir . '/pg_hba.conf';
	open(my $in, '<', $hba) or die;
	my @lines = <$in>;
	close($in);
	open(my $out, '>', $hba) or die;
	print $out $_[0], @lines;
	close($out);
}

o('postgres', q{
	CREATE SCHEMA n VERSIONED;
	CREATE TABLE n.t (id int PRIMARY KEY, v text);
	INSERT INTO n.t SELECT g, 'v' || g FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'one');
});

# a clone, over the connection
l('postgres', 'CREATE DATABASE c');
l('c', qq{SELECT dolt_clone('$url')});
is(l('c', 'SELECT count(*), max(v) FROM n.t'), '200|v99', 'cloned, with its rows');
is(l('c', q{SELECT dolt_hashof('main')}), o('postgres', q{SELECT dolt_hashof('main')}),
	'at the remote\'s head');

# a pull of what the remote committed since
o('postgres', q{
	UPDATE n.t SET v = 'w' WHERE id <= 10;
	SELECT dolt_commit('-am', 'two');
});
l('c', q{SELECT dolt_pull('origin')});
is(l('c', q{SELECT count(*) FROM n.t WHERE v = 'w'}), '10', 'pulled');

# a push: the remote's branch moved, its rows written there
l('c', q{
	INSERT INTO n.t VALUES (201, 'pushed');
	SELECT dolt_commit('-am', 'three');
	SELECT dolt_push('origin', 'main');
});
is(o('postgres', q{SELECT dolt_hashof('main')}), l('c', q{SELECT dolt_hashof('main')}),
	'pushed: the remote\'s head moved');
is(o('postgres', q{SELECT v FROM n.t WHERE id = 201}), 'pushed',
	'pushed: the remote\'s rows');

# uncommitted changes there: refused, unless forced (which drops them,
# and in the owned writer mode the claim of the writer whose they were)
o('postgres', q{
	SELECT dolt_writer_mode('owned');
	SET dolt.writer = 'alice';
	INSERT INTO n.t VALUES (202, 'uncommitted');
});
l('c', q{
	INSERT INTO n.t VALUES (203, 'four');
	SELECT dolt_commit('-am', 'four');
});
my ($ret, $out, $err) = $local->psql('c', q{SELECT dolt_push('origin', 'main')});
like($err, qr/branch "main" has uncommitted changes/, 'refused over uncommitted changes');
is(o('postgres', q{SELECT count(*) FROM n.t WHERE id = 202}), '1', 'and they stay');
l('c', q{SELECT dolt_push('--force', 'origin', 'main')});
is(o('postgres', q{SELECT string_agg(id::text, ',' ORDER BY id) FROM n.t WHERE id > 200}),
	'201,203', 'forced: the push\'s rows, the uncommitted ones gone');
is(o('postgres', q{SELECT coalesce(owner, '') FROM dolt_branches() WHERE name = 'main'}),
	'', 'forced: the uncommitted changes\' writer owns the branch no more');
($ret, $out, $err) = $origin->psql('postgres', q{
	SET dolt.writer = 'bob';
	INSERT INTO n.t VALUES (204, 'bob');
	SELECT owner FROM dolt_branches() WHERE name = 'main';
});
is($out, 'bob', 'and another writer claims it') or diag($err);
o('postgres', q{SET dolt.writer = 'bob'; SELECT dolt_reset('--hard')});

# a push that isn't forced keeps a claim: alice's, her changes undone
o('postgres', q{
	SET dolt.writer = 'alice';
	INSERT INTO n.t VALUES (205, 'undone');
	DELETE FROM n.t WHERE id = 205;
});
l('c', q{
	INSERT INTO n.t VALUES (206, 'five');
	SELECT dolt_commit('-am', 'five');
	SELECT dolt_push('origin', 'main');
});
is(o('postgres', q{SELECT dolt_hashof('main')}), l('c', q{SELECT dolt_hashof('main')}),
	'pushed onto a branch with an owner, but clean');
is(o('postgres', q{SELECT coalesce(owner, '') FROM dolt_branches() WHERE name = 'main'}),
	'alice', 'not forced: its writer keeps the claim');
o('postgres', q{SET dolt.writer = 'alice'; SELECT dolt_reset('--hard')});
o('postgres', q{SELECT dolt_writer_mode('shared')});

# a push into a database with no history yet
o('postgres', 'CREATE DATABASE fresh');
my $fresh = 'postgresql:///fresh?host=' . $origin->host . '&port=' . $origin->port;
l('c', qq{
	SELECT dolt_remote('add', 'fresh', '$fresh');
	SELECT dolt_push('fresh', 'main');
});
is(o('fresh', q{SELECT dolt_hashof('main')}), l('c', q{SELECT dolt_hashof('main')}),
	'pushed into a database without history');
is(o('fresh', 'SELECT count(*) FROM n.t'), '203', 'with its rows');

# a role that isn't a superuser: a password, used, and pg_prolly_fetch
o('postgres', q{CREATE ROLE alice LOGIN PASSWORD 'secret'});
# (alice authenticates with her password; everyone else as before)
prepend_hba("local all alice scram-sha-256\nhost all alice 127.0.0.1/32 scram-sha-256\nhost all alice ::1/128 scram-sha-256\n");
$origin->reload;
l('c', q{CREATE ROLE bob; GRANT pg_prolly_fetch TO bob});
my $alice = 'postgresql:///postgres?host=' . $origin->host . '&port=' . $origin->port .
  '&user=alice';
l('c', qq{
	SELECT dolt_remote('add', 'as_alice', '$alice');
	SELECT dolt_remote('add', 'as_alice_pw', '$alice&password=secret');
	GRANT ALL ON SCHEMA public TO bob;
});
($ret, $out, $err) = $local->psql('c', q{SET ROLE bob; SELECT dolt_fetch('as_alice')});
like($err, qr/password is required/, 'a non-superuser needs a password');
($ret, $out, $err) = $local->psql('c', q{SET ROLE bob; SELECT dolt_fetch('as_alice_pw')});
like($err, qr/permission denied for function dolt_serve_format/,
	'and the remote\'s pg_prolly_fetch');
o('postgres', 'GRANT pg_prolly_fetch TO alice');
($ret, $out, $err) = $local->psql('c', q{SET ROLE bob; SELECT dolt_fetch('as_alice_pw')});
is($ret, 0, 'fetched with both') or diag($err);

# a remote that takes TLS connections only (a build with OpenSSL)
if ($ENV{with_ssl} ne 'openssl')
{
	done_testing();
	exit;
}
my $ssl = $origin->data_dir;
copy("$FindBin::Bin/../../../ssl/ssl/server-cn-only.crt", "$ssl/server.crt") or die;
copy("$FindBin::Bin/../../../ssl/ssl/server-cn-only.key", "$ssl/server.key") or die;
chmod(0600, "$ssl/server.key") or die;
$origin->append_conf('postgresql.conf', qq{ssl = on
listen_addresses = '127.0.0.1'});
prepend_hba("hostnossl all all 127.0.0.1/32 reject\nhostssl all all 127.0.0.1/32 trust\n");
$origin->restart;
my $tcp = 'postgresql://127.0.0.1:' . $origin->port . '/postgres';
l('c', qq{
	SELECT dolt_remote('add', 'plain', '$tcp?sslmode=disable');
	SELECT dolt_remote('add', 'tls', '$tcp?sslmode=require');
});
($ret, $out, $err) = $local->psql('c', q{SELECT dolt_fetch('plain')});
like($err, qr/no encryption/, 'refused without TLS');
($ret, $out, $err) = $local->psql('c', q{SELECT dolt_fetch('tls')});
is($ret, 0, 'fetched over TLS') or diag($err);

done_testing();
