# Copyright (c) 2026, PostgreSQL Global Development Group

# A network remote is whatever server its URL names, so its answers are
# checked before they're read: a result without the rows, columns, types
# or formats asked for, or with a null, is a protocol violation, an ERROR,
# not a crash (#302) or a wrong answer.  The remote here is a fake server,
# speaking just enough of the protocol to accept a connection and answer
# each query with the rows a test gives it.

use strict;
use warnings FATAL => 'all';
use IO::Socket::INET;
use POSIX ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('remote_answers');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres',
	"CREATE DATABASE r ENCODING 'UTF8' LOCALE 'C' TEMPLATE template0");
$node->safe_psql('r', q{
	ALTER DATABASE r SET default_table_access_method = prolly;
	DROP SCHEMA public CASCADE;
	CREATE SCHEMA public VERSIONED;
	CREATE TABLE t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'one');
});
my $format = $node->safe_psql('r', 'SELECT dolt_serve_format()');

# the fakes still running: stopped however the test ends, as they hold
# the harness's output open
my %fakes;

END
{
	foreach my $pid (keys %fakes)
	{
		kill('KILL', $pid);
		waitpid($pid, 0);
	}
}

sub message
{
	my ($type, $body) = @_;
	return $type . pack('N', length($body) + 4) . $body;
}

# A query's answer: its columns (each a name and a type's oid), in format
# $format, and its rows (each a list of values, undef a null), as a
# RowDescription, DataRows and a CommandComplete.
sub answer
{
	my ($cols, $rows, $format) = @_;
	my $out = message('T', pack('n', scalar @$cols)
		  . join('', map { "$_->[0]\0" . pack('NnNnNn', 0, 0, $_->[1], 65535, 4294967295, $format) } @$cols));
	foreach my $row (@$rows)
	{
		$out .= message('D', pack('n', scalar @$row)
			  . join('', map { defined $_ ? pack('N', length($_)) . $_ : pack('N', 4294967295) } @$row));
	}
	return $out . message('C', 'SELECT ' . scalar(@$rows) . "\0");
}

sub read_exactly
{
	my ($sock, $n) = @_;
	my $buf = '';
	while (length($buf) < $n)
	{
		my $got = sysread($sock, $buf, $n - length($buf), length($buf));
		return undef if !$got;
	}
	return $buf;
}

# The result format a Bind asks for (one code for all columns, as libpq
# sends it).
sub bind_format
{
	my ($body) = @_;
	my ($portal, $stmt, $rest) = split(/\0/, $body, 3);
	my $nfmt = unpack('n', $rest);
	$rest = substr($rest, 2 + 2 * $nfmt);
	my $nparams = unpack('n', $rest);
	$rest = substr($rest, 2);
	for (1 .. $nparams)
	{
		my $len = unpack('N', $rest);
		$rest = substr($rest, 4 + ($len == 4294967295 ? 0 : $len));
	}
	my $nres = unpack('n', $rest);
	return $nres > 0 ? unpack('n', substr($rest, 2)) : 0;
}

# A fake server on a free port, in a child process: each query whose text
# matches a rule's pattern gets that rule's answer ([pattern, columns, rows]
# and, if given, the format it answers in, not the one asked for); any
# other, one text column and no rows.  Returns the port.
sub start_fake
{
	my @rules = @_;
	my $port = PostgreSQL::Test::Cluster::get_free_port();
	my $listen = IO::Socket::INET->new(
		LocalAddr => '127.0.0.1',
		LocalPort => $port,
		Listen => 5,
		ReuseAddr => 1) or die "listen: $!";
	my $pid = fork();
	die "fork: $!" if !defined $pid;
	if ($pid)
	{
		close($listen);
		$fakes{$pid} = 1;
		return $port;
	}
	# (the child runs none of the parent's handlers or END blocks, which
	# would stop the cluster, even if it dies)
	$SIG{$_} = 'DEFAULT' foreach qw(TERM INT HUP __DIE__);
	eval {
		while (my $sock = $listen->accept)
		{
			# the startup message, answered: trusted, ready
			my $len = read_exactly($sock, 4);
			next if !defined $len;
			read_exactly($sock, unpack('N', $len) - 4);
			syswrite($sock, message('R', pack('N', 0))
				  . message('S', "server_version\00018.0\0")
				  . message('S', "client_encoding\0UTF8\0")
				  . message('S', "standard_conforming_strings\0on\0")
				  . message('K', pack('NN', $$, 1))
				  . message('Z', 'I'));
			my $sql = '';
			my $asked = 0;
			while (defined(my $head = read_exactly($sock, 5)))
			{
				my ($type, $n) = unpack('aN', $head);
				my $body = $n > 4 ? read_exactly($sock, $n - 4) : '';
				last if $type eq 'X' || !defined $body;
				$sql = (split(/\0/, $body))[1] if $type eq 'P';
				$sql = (split(/\0/, $body))[0] if $type eq 'Q';
				$asked = bind_format($body) if $type eq 'B';
				$asked = 0 if $type eq 'Q';
				next if $type ne 'S' && $type ne 'Q';
				my ($cols, $rows, $fmt) = ([ [ 'x', 25 ] ], [], undef);
				foreach my $rule (@rules)
				{
					(undef, $cols, $rows, $fmt) = @$rule if $sql =~ $rule->[0];
				}
				syswrite($sock, ($type eq 'S' ? message('1', '') . message('2', '') : '')
					  . answer($cols, $rows, $fmt // $asked) . message('Z', 'T'));
			}
			close($sock);
		}
	};
	POSIX::_exit(0);
}

# A fetch from a fake following rules: its error, verbose.  (A timeout, if
# the backend waits on the fake for something it never sends: a failure,
# not a hang.)
my $nremote = 0;

sub fetch_from
{
	my @rules = @_;
	my $name = 'fake' . $nremote++;
	my $port = start_fake(@rules);
	$node->safe_psql('r', qq{SELECT dolt_remote('add', '$name',
		'postgresql://127.0.0.1:$port/x?sslmode=disable&gssencmode=disable&connect_timeout=10')});
	my ($ret, $out, $err) = $node->psql('r', qq{SELECT dolt_fetch('$name')},
		extra_params => [ '-v', 'VERBOSITY=verbose' ],
		timeout => $PostgreSQL::Test::Utils::timeout_default);
	my ($pid) = keys %fakes;
	kill('TERM', $pid);
	waitpid($pid, 0);
	delete $fakes{$pid};
	return $err;
}

my $commit = 'c' x 32;
my @format = (qr/dolt_serve_format/, [ [ 'f', 25 ] ], [ [$format] ]);
my @heads_cols = ([ 'branch', 25 ], [ 'commit_id', 17 ]);

# no rows at all, for every query: the format is the first read
like(fetch_from(),
	qr/08P01: remote .* answered with 0 rows, not 1/,
	'a remote answering without rows: a protocol violation');
is($node->safe_psql('r', 'SELECT 1'), '1', 'and the server still up');

# the format, but heads in one column, not two
like(fetch_from(\@format, [ qr/dolt_serve_heads/, [ [ 'branch', 25 ] ], [] ]),
	qr/08P01: remote .* answered with 1 column, not 2/,
	'a remote answering with too few columns: a protocol violation');

# a head, then nothing when asked whether it serves what changed since
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, \@heads_cols, [ [ 'main', $commit ] ] ],
		[ qr/to_regprocedure/, [ [ 'since', 16 ] ], [] ]),
	qr/08P01: remote .* answered with 0 rows, not 1/,
	'a remote answering nothing about what it serves: a protocol violation');

# a null for the format: not an empty one
like(fetch_from([ qr/dolt_serve_format/, [ [ 'f', 25 ] ], [ [undef] ] ]),
	qr/08P01: remote .* answered with a null in row 1, column 1/,
	'a remote answering with a null: a protocol violation');

# a head's commit as an int8, not a bytea: not read as a hash
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, [ [ 'branch', 25 ], [ 'commit_id', 20 ] ],
		  [ [ 'main', pack('q>', 1) ] ] ]),
	qr/08P01: remote .* answered with type 20 in column 2, not 17/,
	'a remote answering with another type: a protocol violation');

# heads in text, not the binary asked for
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, \@heads_cols, [ [ 'main', $commit ] ], 0 ]),
	qr/08P01: remote .* answered with format 0 in column 1, not 1/,
	'a remote answering in another format: a protocol violation');

# a branch's name with a NUL in it, or one no rules made (empty, HEAD, a
# control character): refused, naming the remote and the name
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, \@heads_cols, [ [ "ma\0in", $commit ] ] ]),
	qr/08P01: remote .* sent a branch name with a NUL in it/,
	'a remote sending a branch name with a NUL: a protocol violation');
foreach my $bad ('HEAD', "a\x01b", "a\x7fb", '')
{
	(my $shown = $bad) =~ s/([^ -~])/sprintf('\\x%02x', ord($1))/ge;
	like(fetch_from(\@format,
			[ qr/dolt_serve_heads/, \@heads_cols, [ [ $bad, $commit ] ] ]),
		qr/08P01: remote "fake\d+" sent the branch name "\Q$shown\E", which no branch can have/,
		"a remote sending the branch name \"$shown\": a protocol violation");
}

# one not valid in this database's encoding: that head alone not fetched,
# with a warning naming it
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, \@heads_cols, [ [ "ma\xffin", $commit ] ] ]),
	qr/WARNING:  22021: remote "fake\d+"'s branch "ma\\xffin" isn't fetched/,
	'a remote sending a branch name not of the encoding: skipped, with a warning');
is($node->safe_psql('r', 'SELECT count(*) FROM dolt_remote_branches'), '0',
	'and nothing tracked');

# branches made under older rules ("head", one not ASCII): tracked, and
# a clone warns, not making branches of them; a fetch and a push work
my $legacy = 'file://' . PostgreSQL::Test::Utils::tempdir() . '/legacy';
$node->safe_psql('r', qq{
	SET client_min_messages = warning;
	CREATE SCHEMA tp;
	CREATE EXTENSION test_prolly SCHEMA tp;
	SELECT dolt_remote('add', 'legacy', '$legacy');
	SELECT dolt_push('legacy', 'main');
	SELECT tp.test_remote_set_head('$legacy', name, dolt_hashof('main'),
		(SELECT value FROM tp.test_fold_items() WHERE key = '!:format'))
	FROM (VALUES ('head'), ('caf\x{c3}\x{a9}')) AS v(name);
});
$node->safe_psql('postgres',
	"CREATE DATABASE c ENCODING 'UTF8' LOCALE 'C' TEMPLATE template0");
$node->safe_psql('c', 'DROP SCHEMA public');
my ($ret, $out, $err) = $node->psql('c', qq{SELECT dolt_clone('$legacy')},
	timeout => $PostgreSQL::Test::Utils::timeout_default);
is($ret, 0, 'a remote with legacy branch names cloned');
like($err, qr/WARNING:  remote branch "head" isn't cloned as a branch/,
	'its "head" tracked, with a warning');
like($err, qr/WARNING:  remote branch "caf\x{c3}\x{a9}" isn't cloned as a branch/,
	'its non-ASCII branch tracked, with a warning');
is($node->safe_psql('c', qq{SELECT count(*) FROM dolt_remote_branches
	WHERE name IN ('remotes/origin/head', 'remotes/origin/caf\x{c3}\x{a9}')}), '2', 'both tracked');
$node->safe_psql('r', q{
	SELECT dolt_fetch('legacy');
	SELECT dolt_push('legacy', 'main');
});
pass('a fetch from it, and a push to it');

# a head, read a chunk at a time, and two chunks for the one asked for
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, \@heads_cols, [ [ 'main', $commit ] ] ],
		[ qr/to_regprocedure/, [ [ 'since', 16 ] ], [ ['f'] ] ],
		[ qr/dolt_serve_has/, [ [ 'has', 16 ] ], [ ['t'] ] ],
		[ qr/SELECT hash, chunk/, [ [ 'hash', 17 ], [ 'chunk', 17 ] ], [] ],
		[ qr/SELECT chunk FROM/, [ [ 'chunk', 17 ] ], [ ['a'], ['b'] ] ]),
	qr/08P01: remote .* answered with 2 rows, not 0 to 1/,
	'a remote answering one chunk with two: a protocol violation');

# what changed since, streamed: a chunk this database has, then a null
my $have = pack('H*', $node->safe_psql('r', q{SELECT dolt_hashof('main')}));
like(fetch_from(\@format,
		[ qr/dolt_serve_heads/, \@heads_cols, [ [ 'main', $commit ] ] ],
		[ qr/FROM pg_catalog.dolt_serve_since/, [ [ 'hash', 17 ], [ 'chunk', 17 ] ],
		  [ [ $have, 'x' ], [ 'd' x 32, undef ] ] ],
		[ qr/to_regprocedure/, [ [ 'since', 16 ] ], [ ['t'] ] ]),
	qr/08P01: remote .* answered with a null in row 1, column 2/,
	'a remote streaming a null after its first row: a protocol violation');

ok(!$node->log_contains(qr/terminated by signal/), 'no backend crashed');

# a remote in another encoding, with no conversion to this one's: still
# one to clone
$node->safe_psql('postgres',
	"CREATE DATABASE w ENCODING 'WIN1251' LOCALE 'C' TEMPLATE template0");
$node->safe_psql('w', q{
	CREATE SCHEMA n VERSIONED;
	CREATE TABLE n.t (id int PRIMARY KEY, v text);
	INSERT INTO n.t VALUES (1, 'one');
	SELECT dolt_commit('-Am', 'one');
});
$node->safe_psql('postgres',
	"CREATE DATABASE l ENCODING 'LATIN1' LOCALE 'C' TEMPLATE template0");
my $wurl = 'postgresql:///w?host=' . $node->host . '&port=' . $node->port;
$node->safe_psql('l', qq{SELECT dolt_clone('$wurl')},
	timeout => $PostgreSQL::Test::Utils::timeout_default);
is($node->safe_psql('l', 'SELECT v FROM n.t'), 'one',
	'a WIN1251 remote cloned into a LATIN1 database');

done_testing();
