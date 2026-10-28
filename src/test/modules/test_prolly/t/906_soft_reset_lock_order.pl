# Copyright (c) 2026, PostgreSQL Global Development Group

# A soft reset holds its branch before it reads the head it resolves its
# target from: a commit of the branch that lands while the reset waits is
# the head it steps back from, not one it moves the head back past, and
# two soft resets of HEAD~1, one waiting for the other, step back twice.
# A hard reset holds it as well before it checks what the head is (a
# protected branch moved to another commit is the database owner's).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('soft_reset_lock_order');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE sr');

sub sql { return $node->safe_psql('sr', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY)});
for my $i (1 .. 4)
{
	sql("INSERT INTO app.t VALUES ($i); SELECT dolt_commit('-Am', 'c$i')");
}

sub head { return sql(q{SELECT dolt_hashof('HEAD')}); }

# (a session waiting for a lock, as pg_locks has it)
sub waits_for_lock
{
	my ($pid) = @_;
	return $node->poll_query_until('sr',
		qq{SELECT count(*) > 0 FROM pg_locks WHERE pid = $pid AND NOT granted});
}

# A commit in progress holds the branch; a soft reset waits for it.
my $c4 = head();
my $n4 = sql(q{SELECT count(*) FROM dolt_log()});
my $committer = $node->background_psql('sr');
$committer->query_safe(q{BEGIN; INSERT INTO app.t VALUES (5);
	SELECT dolt_commit('-am', 'c5')});
my $c5 = $committer->query_safe(q{SELECT dolt_hashof('HEAD')});
chomp $c5;
my $resetter = $node->background_psql('sr');
my $rpid = $resetter->query_safe('SELECT pg_backend_pid()');
chomp $rpid;
$resetter->query_until(qr/started/, q{\echo started
SELECT dolt_reset('--soft', 'HEAD~1') IS NOT NULL AS reset;
\echo reset
});
ok(waits_for_lock($rpid), 'the soft reset waits for the commit in progress');
$committer->query_safe('COMMIT');
$resetter->query_until(qr/reset/, '');
is(head(), $c4, 'it steps back from the commit that landed meanwhile');
is(sql(qq{SELECT count(*) FROM dolt_log() WHERE commit_hash = '$c5'}), '0',
	'which it stepped back past, as a reset of it does');
is(sql(q{SELECT count(*) FROM dolt_log()}), $n4,
	'and no commit before it is lost');
$committer->quit;
$resetter->quit;

# Two soft resets of HEAD~1: the second waits for the first, then steps
# back from where the first left the head.
my $c3 = sql(q{SELECT dolt_hashof('HEAD~1')});
my $c2 = sql(q{SELECT dolt_hashof('HEAD~2')});
my $first = $node->background_psql('sr');
$first->query_safe(q{BEGIN; SELECT dolt_reset('--soft', 'HEAD~1')});
my $second = $node->background_psql('sr');
my $spid = $second->query_safe('SELECT pg_backend_pid()');
chomp $spid;
$second->query_until(qr/started/, q{\echo started
SELECT dolt_reset('--soft', 'HEAD~1') IS NOT NULL AS reset;
\echo reset
});
ok(waits_for_lock($spid), 'the second soft reset waits for the first');
$first->query_safe('COMMIT');
$second->query_until(qr/reset/, '');
is(head(), $c2, 'two soft resets of HEAD~1 step back twice');
isnt(head(), $c3, 'not once');
$first->quit;
$second->quit;

# A hard reset by a role that isn't the database's owner, to the head, of
# a protected branch, waiting behind the owner's move of it: its check is
# of the head the owner left, so it is refused as a move of the branch to
# another commit, not run as one to its head.
sql(q{CREATE ROLE regress_w IN ROLE pg_prolly_write;
	GRANT USAGE ON SCHEMA app TO regress_w;
	GRANT ALL ON app.t TO regress_w;
	SELECT dolt_commit('-Am', 'granted')});
sql(q{SELECT dolt_reset('--hard'); SELECT dolt_branch('--protect', 'main')});
my $old = head();
my $owner = $node->background_psql('sr');
$owner->query_safe(q{BEGIN; SELECT dolt_reset('--hard', 'HEAD~1')});
my $moved_to = $owner->query_safe(q{SELECT dolt_hashof('HEAD')});
chomp $moved_to;
my $writer = $node->background_psql('sr', on_error_stop => 0);
my $wpid = $writer->query_safe('SELECT pg_backend_pid()');
chomp $wpid;
$writer->query_until(qr/started/, qq{\\echo started
SET ROLE regress_w;
SELECT dolt_reset('--hard', '$old') IS NOT NULL AS moved;
\\echo done
});
ok(waits_for_lock($wpid), 'the hard reset waits for the owner\'s move');
$owner->query_safe('COMMIT');
$writer->query_until(qr/done/, '');
like($writer->{stderr}, qr/branch "main" is protected/,
	'then is refused: the protected branch\'s head moved since it was asked');
is(head(), $moved_to, 'the head stays where the owner moved it');
$owner->quit;
$writer->quit;

$node->stop;
done_testing();
