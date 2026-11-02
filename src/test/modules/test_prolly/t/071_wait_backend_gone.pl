
# Copyright (c) 2026, PostgreSQL Global Development Group

# test_wait_backend_gone, which tests wait on for a session to end: it
# sees a backend that ends after it starts waiting, not only one gone
# before (pg_stat_activity is a snapshot each transaction).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('wait_gone');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $s = $node->background_psql('postgres');
my $pid = $s->query_safe('SELECT pg_backend_pid()');
my $w = $node->background_psql('postgres');
$w->query_until(qr/waiting/, "\\echo waiting\nSELECT test_wait_backend_gone($pid) AS gone;\n");
# (the wait has looked, the session there still, before it ends)
usleep(300_000);
$s->quit;
my $start = time;
$w->{stdin} .= "\\echo done\n";
$w->{run}->pump until $w->{stdout} =~ /done/ || time - $start > 30;
like($w->{stdout}, qr/\bt\b/, 'a session ended after the wait began is seen gone');
ok(time - $start < 30, 'and soon');
$w->quit;
$node->stop;
done_testing();
