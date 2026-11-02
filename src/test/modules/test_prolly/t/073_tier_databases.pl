
# Copyright (c) 2026, PostgreSQL Global Development Group

# Databases and the history store's tier: a database whose tier has packs
# is not dropped, nor copied as a template (its objects would be left, or
# named by two databases); emptied, it is; and a database copied from one
# whose tier had packs, all removed, takes a stamp of its own, its objects
# none of the other's.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tier = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('tier_dbs');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
history_tier_location = '$tier'
});
$node->start;
my $objects = sub {
	opendir(my $dh, $tier) or return ();
	my @o = sort grep(!/^\./, readdir($dh));
	closedir($dh);
	return @o;
};
my $setup = q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
};
$node->safe_psql('postgres', 'CREATE DATABASE d1');
$node->safe_psql('d1', $setup . q{
	SELECT (test_cs_put('tier', '\x0a7301')).stored;
	SELECT test_cs_ref_root('tier', h('\x0a7301'));
});
ok(scalar(grep(/^databases#/, $objects->())) == 1, 'a marker, the tier having a pack');

my ($ret, $out, $err) = $node->psql('postgres', 'DROP DATABASE d1');
like($err, qr/cannot drop database "d1": its history store's tier has packs/, 'DROP DATABASE refused');
($ret, $out, $err) = $node->psql('postgres', 'CREATE DATABASE d2 TEMPLATE d1');
like($err, qr/cannot copy database "d1": its history store's tier has packs/, 'and copying it');

# emptied: the marker removed, and the database copied, then dropped
$node->safe_psql('d1', q{
	SELECT test_cs_unref_root('tier', h('\x0a7301'));
	SELECT test_cs_free('tier', ARRAY[h('\x0a7301')]);
	VACUUM pg_history.tier_index;
});
is(scalar(grep(/^databases#/, $objects->())), 0, 'emptied: no marker');
$node->safe_psql('postgres', 'CREATE DATABASE d2 TEMPLATE d1');

# the copy's tier, its packs all removed, takes a stamp of its own: a pack
# of each database, each read there, neither the other's
$node->safe_psql('d1', q{
	SELECT (test_cs_put('tier', '\x0a7302')).stored;
	SELECT test_cs_ref_root('tier', h('\x0a7302'));
});
my @before = grep(/^packs#/, $objects->());
$node->safe_psql('d2', q{
	SELECT (test_cs_put('tier', '\x0a7303')).stored;
	SELECT test_cs_ref_root('tier', h('\x0a7303'));
});
my @after = grep(/^packs#/, $objects->());
is(scalar(@after), scalar(@before) + 1, 'a pack of each');
my %stamps = map { (split(/#/, $_))[1] => 1 } @after;
is(scalar(keys %stamps), 2, 'of two stamps');
is($node->safe_psql('d1', q{SELECT test_cs_get('tier', h('\x0a7302'))}), '\x0a7302', 'd1 reads its own');
is($node->safe_psql('d2', q{SELECT test_cs_get('tier', h('\x0a7303'))}), '\x0a7303', 'd2 reads its own');

# d2 emptied and dropped; d1 reads its own still
$node->safe_psql('d2', q{
	SELECT test_cs_unref_root('tier', h('\x0a7303'));
	SELECT test_cs_free('tier', ARRAY[h('\x0a7303')]);
	VACUUM pg_history.tier_index;
});
$node->safe_psql('postgres', 'DROP DATABASE d2');
is($node->safe_psql('d1', q{SELECT test_cs_get('tier', h('\x0a7302'))}), '\x0a7302', 'd1 reads its own still');
($ret, $out, $err) = $node->psql('postgres', 'DROP DATABASE d1');
like($err, qr/its history store's tier has packs/, 'd1, with a pack, not dropped');

# a pack's beginning cut short after the marker is made (an error as the
# catalog's file is extended): the tier has no pack, and the marker stays;
# the tier dropped, empty, takes the marker with it, and the database goes
SKIP:
{
	skip 'injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', 'CREATE DATABASE dx');
	$node->safe_psql('dx', $setup . q{
		CREATE EXTENSION injection_points;
		SET allow_system_table_mods = on;
		CREATE TABLE pg_history.t () USING prolly_tier;
		CREATE TABLE pg_history.ti () USING prolly_chunk_index;
	});
	my ($r, $o, $e) = $node->psql('dx', q{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('chunk-tier-page-extended', 'error');
		SELECT test_chunk_put('pg_history.t', 'pg_history.ti', h('\x0a7309'), '\x0a7309');
	});
	like($e, qr/error triggered for injection point/, 'a pack begun, cut short');
	$node->safe_psql('dx', q{
		SET allow_system_table_mods = on;
		DROP TABLE pg_history.t, pg_history.ti;
	});
	$node->safe_psql('postgres', 'DROP DATABASE dx');
	pass('the empty tier dropped took its marker: the database dropped');
}

# the tier's place changed with packs in it: VACUUM and puts refused, the
# packs not taken for gone; set back, all as it was
my $tier2 = PostgreSQL::Test::Utils::tempdir;
$node->append_conf('postgresql.conf', qq{history_tier_location = '$tier2'});
$node->reload;
$node->poll_query_until('d1', qq{SELECT current_setting('history_tier_location') = '$tier2'})
  or die 'location not reloaded';
($ret, $out, $err) = $node->psql('d1', 'VACUUM pg_history.tier_index');
like($err, qr/objects are in another place than history_tier_store and history_tier_location say/, 'VACUUM refused elsewhere');
($ret, $out, $err) = $node->psql('d1', q{SELECT (test_cs_put('tier', '\x0a7304')).stored});
like($err, qr/objects are in another place/, 'and a put');
$node->append_conf('postgresql.conf', qq{history_tier_location = '$tier'});
$node->reload;
$node->poll_query_until('d1', qq{SELECT current_setting('history_tier_location') = '$tier'})
  or die 'location not reloaded';
$node->safe_psql('d1', 'VACUUM pg_history.tier_index');
is($node->safe_psql('d1', q{SELECT test_cs_get('tier', h('\x0a7302'))}), '\x0a7302', 'set back: read as it was');

$node->stop;
done_testing();
