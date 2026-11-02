
# Copyright (c) 2026, PostgreSQL Global Development Group

# A bundle's manifest written, the push cut short by a crash before its
# size is recorded or its transaction commits: an orphan, its object
# removed by VACUUM, listed no more.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $tier = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('manifest_crash');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
history_tier_location = '$tier'
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	SELECT (test_cs_put('pages', '\x0a7401')).stored;
});
my $manifests = sub {
	opendir(my $dh, $tier) or return 0;
	my $n = grep(/^manifests#/, readdir($dh));
	closedir($dh);
	return $n;
};

my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe(q{SELECT injection_points_set_local()});
$s->query_safe(q{SELECT injection_points_attach('chunk-tier-manifest-written', 'wait')});
$s->query_until(qr/pushing/, "\\echo pushing\nSELECT pg_history_push('bc', ARRAY[h('\\x0a7401')]);\n");
$node->wait_for_event('client backend', 'chunk-tier-manifest-written');
is($manifests->(), 1, 'the manifest written');
$node->stop('immediate');
$s->{run}->finish;
$node->start;

is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_history.bundles}), '0', 'no bundle, the push cut short');
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($manifests->(), 0, 'its manifest, an orphan, removed');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_history_tier_bundles()}), '0', 'and listed no more');

$node->stop;
done_testing();
