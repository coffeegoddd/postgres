
# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade carries each database's version-control history store over:
# its relations' files (the chunk log, chunk index, pointers, tier and
# bundles, their TOAST tables and indexes) the old cluster's, by every way
# it moves files, as pg_largeobject's, whatever file numbers they have come
# to; a tier's pack objects stay where they are, the new cluster's to read,
# its marker named by the new system, the old system's removed, and the old
# cluster disabled.  A new cluster whose settings name another place for
# the tiers' objects is refused.
use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# (pg_upgrade writes its files in the directory it runs in)
chdir PostgreSQL::Test::Utils::tempdir;

my $fns = q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('u' || i), 20), 'hex')$$;
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
	CREATE FUNCTION tree(a int, b int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT node(VARIADIC ARRAY(SELECT leaf(i) FROM generate_series(a, b) i))';
	-- a pointer's tree of leaves 1-20, a bundle's of 11-30 (pushed)
	SELECT count(*) FROM generate_series(1, 30) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT (test_cs_put('pages', tree(1, 20))).stored, (test_cs_put('pages', tree(11, 30))).stored;
	INSERT INTO pg_history.pointers VALUES ('main', h(tree(1, 20)));
	SELECT test_cs_ref_root('pages', h(tree(11, 30)));
	BEGIN;
	SELECT test_cs_unref_root('pages', h(tree(11, 30)));
	SELECT pg_history_push('b1', ARRAY[h(tree(11, 30))]);
	COMMIT;
};
my $conf = sub {
	my ($node, $tier) = @_;
	$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
test_prolly.chunk_format_refs = on
autovacuum = off
chunk_free_in_background = off
history_tier_location = '$tier'
});
};
my $markers = sub {
	my ($tier, $sysid) = @_;
	my $hex = sprintf('%016x', $sysid);
	# (the directory store's objects, their names' slashes '#')
	return scalar(my @m = glob("$tier/databases#$hex#*#*"));
};

# (--copy of clusters of the same system identifier, both copies of one
# initdb's, the old one's markers the new one's names; the others of
# their own)
for my $mode ('--copy', '--link', '--swap')
{
	(my $suffix = $mode) =~ s/^--/_/;
	my $own = $mode ne '--copy';
	my $tier = PostgreSQL::Test::Utils::tempdir;
	my $old = PostgreSQL::Test::Cluster->new("history_old$suffix");
	$old->init(force_initdb => $own);
	$conf->($old, $tier);
	$old->start;
	$old->safe_psql('postgres', 'CREATE DATABASE other');
	$old->safe_psql($_, $fns) for ('postgres', 'other');
	# (the pointers' and bundles' files numbered apart from their OIDs)
	$old->safe_psql('postgres', q{
		VACUUM FULL pg_history.pointers;
		REINDEX TABLE pg_history.bundles;
		VACUUM FREEZE pg_history.pointers;
	});
	my $old_sysid = $old->safe_psql('postgres', 'SELECT system_identifier FROM pg_control_system()');
	my $files = q{SELECT string_agg(c.relname || ':' || (c.relfilenode <> c.oid), ',' ORDER BY c.relname)
		FROM pg_class c WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relkind IN ('r', 'i')};
	like($old->safe_psql('postgres', $files), qr/pointers:true/, "$mode: the old pointers' file renumbered");
	my $horizon = q{SELECT relfrozenxid FROM pg_class WHERE oid = 'pg_history.pointers'::regclass};
	my $old_horizon = $old->safe_psql('postgres', $horizon);
	# (each relation's file number, its TOAST table's and that one's index's)
	my $numbers = q{SELECT string_agg(c.relname || ':' || c.relfilenode || ':' ||
			coalesce(t.relfilenode, 0) || ':' || coalesce(ti.relfilenode, 0), ',' ORDER BY c.relname)
		FROM pg_class c LEFT JOIN pg_class t ON t.oid = c.reltoastrelid
			LEFT JOIN pg_index i ON i.indrelid = t.oid LEFT JOIN pg_class ti ON ti.oid = i.indexrelid
		WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relkind IN ('r', 'i')};
	my $old_numbers = $old->safe_psql('postgres', $numbers);
	is($markers->($tier, $old_sysid), 2, "$mode: the old system's markers, a database each");
	$old->stop;

	my $new = PostgreSQL::Test::Cluster->new("history_new$suffix");
	$new->init(force_initdb => $own);
	$conf->($new, $tier);
	my $bindir = $new->config_data('--bindir');
	my @upgrade = (
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $bindir,
		'--new-bindir' => $bindir,
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		$mode);
	command_ok(\@upgrade, "pg_upgrade $mode of a cluster with history");
	ok(-f $old->data_dir . '/global/pg_control.old', "$mode: the old cluster disabled");

	$new->start;
	my $new_sysid = $new->safe_psql('postgres', 'SELECT system_identifier FROM pg_control_system()');
	for my $db ('postgres', 'other')
	{
		is($new->safe_psql($db, q{SELECT test_cs_get('pages', h(tree(1, 20))) = tree(1, 20)}), 't',
			"$mode, $db: the pointer's tree read");
		is($new->safe_psql($db, q{SELECT name || ':' || (hash = h(tree(1, 20))) FROM pg_history.pointers}),
			'main:true', "$mode, $db: the pointer");
		is($new->safe_psql($db, q{SET enable_seqscan = off;
			SELECT string_agg(name, ',') FROM pg_history.bundles WHERE name = 'b1'}), 'b1',
			"$mode, $db: the bundle, found by its index");
		is($new->safe_psql($db, q{SELECT pg_history_fetch('b1')}), '11',
			"$mode, $db: fetched from the tier's objects, where they were");
		is($new->safe_psql($db, q{SELECT (SELECT ok FROM test_cs_check('pages')) AND (SELECT ok FROM test_cs_check('tier'))}),
			't', "$mode, $db: the audits clean");
	}
	is($new->safe_psql('postgres', $numbers), $old_numbers, "$mode: the relations' file numbers kept");
	is($new->safe_psql('postgres', $horizon), $old_horizon, "$mode: the pointers' relfrozenxid kept");
	is($new_sysid ne $old_sysid, $own, "$mode: the systems' identifiers");
	is($markers->($tier, $new_sysid), 2, "$mode: the new system's markers");
	is($markers->($tier, $old_sysid), 0, "$mode: the old system's removed") if $own;
	my ($ret, $out, $err) = $new->psql('postgres', 'DROP DATABASE other');
	like($err, qr/tier/, "$mode: a database whose tier has packs not dropped");
	$new->stop;
}

# a new cluster whose tiers' place is another: refused
my $tier = PostgreSQL::Test::Utils::tempdir;
my $old = PostgreSQL::Test::Cluster->new('history_old_place');
$old->init;
$conf->($old, $tier);
$old->start;
$old->safe_psql('postgres', $fns);
$old->stop;
my $new = PostgreSQL::Test::Cluster->new('history_new_place');
$new->init;
$conf->($new, PostgreSQL::Test::Utils::tempdir);
my $bindir = $new->config_data('--bindir');
my ($stdout, $stderr) = run_command([
	'pg_upgrade', '--no-sync', '--check',
	'--old-datadir' => $old->data_dir,
	'--new-datadir' => $new->data_dir,
	'--old-bindir' => $bindir,
	'--new-bindir' => $bindir,
	'--socketdir' => $new->host,
	'--old-port' => $old->port,
	'--new-port' => $new->port]);
like($stdout . $stderr, qr/and the new cluster's settings name another place/,
	'a new cluster whose tiers\' place is another refused');

done_testing();
