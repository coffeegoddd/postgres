
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
		AS $$SELECT test_engine_chunk('L'::bytea || decode(repeat(md5('u' || i), 20), 'hex'))$$;
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT test_engine_chunk('R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o)))$$;
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

# (each mode of clusters of their own, and --copy of two of the same
# system identifier, copies of one initdb's, the old one's markers the new
# one's names)
for my $run ([ '--copy', 0, '_copy' ], [ '--copy', 1, '_copy_own' ],
	[ '--link', 1, '_link' ], [ '--swap', 1, '_swap' ])
{
	my ($mode, $own, $suffix) = @$run;
	my $tier = PostgreSQL::Test::Utils::tempdir;
	my $old = PostgreSQL::Test::Cluster->new("history_old$suffix");
	$old->init(force_initdb => $own);
	$conf->($old, $tier);
	$old->start;
	$old->safe_psql('postgres', 'CREATE DATABASE other');
	# (the store's own relations' files numbered apart from their OIDs too,
	# emptied before any is put)
	$old->safe_psql('other', q{
		SET allow_system_table_mods = on;
		TRUNCATE pg_history.tier_index, pg_history.tier_chunks,
			pg_history.chunk_index, pg_history.chunks;
	});
	$old->safe_psql($_, $fns) for ('postgres', 'other');
	# (a second writer at once: a chunk in a lane beside the log's, made
	# as the backend wrote, not by initdb)
	my $holder = $old->background_psql('postgres');
	$holder->query_safe(q{SELECT (test_cs_put('pages', test_engine_chunk('\x0a7701'))).stored});
	$old->safe_psql('postgres', q{
		SELECT (test_cs_put('pages', test_engine_chunk('\x0a7702'))).stored;
		SELECT test_cs_ref_root('pages', h(test_engine_chunk('\x0a7702')));
	});
	$holder->quit;
	is($old->safe_psql('postgres', q{SELECT count(*) > 0 FROM pg_class
		WHERE relnamespace = 'pg_history'::regnamespace AND relname LIKE 'chunks_lane_%'}),
		't', "$mode: the old store has a lane beside its log");
	# (the pointers' and bundles' files numbered apart from their OIDs)
	$old->safe_psql('postgres', q{
		VACUUM FULL pg_history.pointers;
		REINDEX TABLE pg_history.bundles;
		VACUUM FREEZE pg_history.pointers;
	});
	# (the store's indexes not reindexed concurrently, which would give
	# them OIDs anew: refused named, passed by in a database's)
	if ($mode eq '--link')
	{
		my $toast_index = $old->safe_psql('postgres', q{SELECT i.indexrelid::regclass::text
			FROM pg_class c JOIN pg_index i ON i.indrelid = c.reltoastrelid
			WHERE c.oid = 'pg_history.bundles'::regclass});
		for my $what ('INDEX pg_history.pointers_pkey', 'TABLE pg_history.bundles',
			"INDEX $toast_index", 'SCHEMA pg_history')
		{
			my ($kind, $name) = split / /, $what;
			my ($ret, $out, $err) = $old->psql('postgres', "REINDEX $kind CONCURRENTLY $name");
			like($err, qr/cannot reindex system catalogs concurrently/,
				"$mode: REINDEX $kind CONCURRENTLY $name refused, or passed by");
		}
		my ($ret, $out, $err) = $old->psql('postgres', 'REINDEX DATABASE CONCURRENTLY postgres');
		is($ret, 0, "$mode: REINDEX DATABASE CONCURRENTLY, the store passed by");
		$old->safe_psql('postgres', 'REINDEX TABLE pg_history.pointers');
	}
	my $old_sysid = $old->safe_psql('postgres', 'SELECT system_identifier FROM pg_control_system()');
	my $files = q{SELECT string_agg(c.relname || ':' || (c.relfilenode <> c.oid), ',' ORDER BY c.relname)
		FROM pg_class c WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relkind IN ('r', 'i')};
	like($old->safe_psql('postgres', $files), qr/pointers:true/, "$mode: the old pointers' file renumbered");
	like($old->safe_psql('other', $files), qr/chunk_index:true,chunks:true,.*tier_chunks:true,tier_index:true/,
		"$mode: the old store's own relations' files renumbered");
	# (each table's horizons, and its TOAST table's)
	my $horizon = q{SELECT string_agg(c.relname || ':' || c.relfrozenxid || ':' || c.relminmxid || ':' ||
			coalesce(t.relfrozenxid::text, '-') || ':' || coalesce(t.relminmxid::text, '-'), ',' ORDER BY c.relname)
		FROM pg_class c LEFT JOIN pg_class t ON t.oid = c.reltoastrelid
		WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relkind = 'r'};
	# (each relation's file number, its TOAST table's and that one's index's)
	my $numbers = q{SELECT string_agg(c.relname || ':' || c.relfilenode || ':' ||
			coalesce(t.relfilenode, 0) || ':' || coalesce(ti.relfilenode, 0), ',' ORDER BY c.relname)
		FROM pg_class c LEFT JOIN pg_class t ON t.oid = c.reltoastrelid
			LEFT JOIN pg_index i ON i.indrelid = t.oid LEFT JOIN pg_class ti ON ti.oid = i.indexrelid
		WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relkind IN ('r', 'i')};
	my %old_numbers = map { $_ => $old->safe_psql($_, $numbers) } ('postgres', 'other');
	my %old_horizon = map { $_ => $old->safe_psql($_, $horizon) } ('postgres', 'other');
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
	for my $db ('postgres', 'other')
	{
		is($new->safe_psql($db, $numbers), $old_numbers{$db}, "$mode, $db: the relations' file numbers kept");
		if ($db eq 'postgres')
		{
			is($new->safe_psql($db, q{SELECT test_cs_get('pages', h(test_engine_chunk('\x0a7702'))) = test_engine_chunk('\x0a7702')}), 't',
				"$mode: a chunk in a lane read back");
			is($new->safe_psql($db, q{SELECT count(*) FROM pg_class c JOIN pg_depend d ON d.objid = c.oid
				WHERE c.relname LIKE 'chunks_lane_%' AND d.deptype = 'i'
					AND d.refobjid = 'pg_history.chunks'::regclass}) > 0 ? 't' : 'f', 't',
				"$mode: the lanes their log's again");
		}
		is($new->safe_psql($db, $horizon), $old_horizon{$db}, "$mode, $db: their relfrozenxids and relminmxids kept");
	}
	is($new_sysid ne $old_sysid ? 1 : 0, $own, "$mode: the systems' identifiers");
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
like($stdout . $stderr, qr/and the new cluster's settings name no place holding them/,
	'a new cluster whose tiers\' place holds none of them refused');

my $upgrade = sub {
	my ($old, $new, @args) = @_;
	my $bindir = $new->config_data('--bindir');
	return run_command([
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $bindir,
		'--new-bindir' => $bindir,
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		@args]);
};

# an old cluster on a timeline after the first (a standby promoted): its
# packs, of an owner taken on that timeline, the new cluster's own still,
# though it starts on a timeline of its own (a bundle released there, its
# packs removed)
{
	my $tier = PostgreSQL::Test::Utils::tempdir;
	my $primary = PostgreSQL::Test::Cluster->new('history_tl_primary');
	$primary->init(allows_streaming => 1, force_initdb => 1);
	$conf->($primary, $tier);
	$primary->start;
	$primary->safe_psql('postgres', $fns);
	$primary->backup('b');
	my $old = PostgreSQL::Test::Cluster->new('history_tl_standby');
	$old->init_from_backup($primary, 'b', has_streaming => 1);
	$old->start;
	$primary->wait_for_catchup($old);
	$primary->stop;
	$old->promote;
	$old->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()');
	is($old->safe_psql('postgres', q{SELECT timeline_id FROM pg_control_checkpoint()}) > 1 ? 1 : 0, 1,
		'a promoted standby, on a timeline after the first');
	my $before = $old->safe_psql('postgres', q{SELECT max(id) FROM test_tier_packs('pg_history.tier_chunks')});
	$old->safe_psql('postgres', q{
		SELECT count(*) FROM generate_series(41, 45) i, LATERAL test_cs_put('pages', leaf(i));
		SELECT (test_cs_put('pages', tree(41, 45))).stored;
		SELECT test_cs_ref_root('pages', h(tree(41, 45)));
		BEGIN;
		SELECT test_cs_unref_root('pages', h(tree(41, 45)));
		SELECT pg_history_push('b2', ARRAY[h(tree(41, 45))]);
		COMMIT;
	});
	$old->stop;
	my $new = PostgreSQL::Test::Cluster->new('history_tl_new');
	$new->init(force_initdb => 1);
	$conf->($new, $tier);
	my ($stdout) = $upgrade->($old, $new, '--link');
	like($stdout, qr/Upgrade Complete/, 'the promoted standby upgraded');
	$new->start;
	$new->safe_psql('postgres', q{SELECT pg_history_release('b2')});
	$new->safe_psql('postgres', 'VACUUM pg_history.tier_index') for 1 .. 2;
	$new->safe_psql('postgres', 'VACUUM pg_history.tier_chunks') for 1 .. 2;
	is($new->safe_psql('postgres', qq{SELECT count(*) FROM test_tier_packs('pg_history.tier_chunks')
		WHERE id > $before AND state <> 'removed'}), '0',
		'the bundle pushed on that timeline, released: its packs removed, the new cluster\'s own');
	$new->stop;
}

# a tier with no packs left but still marked (VACUUM stopped as it was to
# remove its marker), of clusters of one system identifier: its marker
# removed, and its database dropped
SKIP:
{
	skip 'injection points not supported by this build', 5
		if $ENV{enable_injection_points} ne 'yes';
	my $tier = PostgreSQL::Test::Utils::tempdir;
	my $old = PostgreSQL::Test::Cluster->new('history_old_marked');
	$old->init;
	$conf->($old, $tier);
	$old->start;
	$old->safe_psql('postgres', 'CREATE DATABASE em');
	$old->safe_psql('em', $fns);
	my $em = $old->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'em'});
	# (VACUUM of the tier's index ends with its tier's)
	$old->safe_psql('em', q{
		SELECT pg_history_release('b1');
		CREATE EXTENSION injection_points;
		SELECT injection_points_attach('chunk-tier-marker-remove', 'error');
	});
	my $err = '';
	for (1 .. 3)
	{
		my ($ret, $out);
		($ret, $out, $err) = $old->psql('em', 'VACUUM pg_history.tier_index');
		last if $err =~ /chunk-tier-marker-remove/;
	}
	like($err, qr/chunk-tier-marker-remove/, 'VACUUM stopped as it was to remove the marker');
	is(scalar(my @m = glob("$tier/databases#*#$em#*")), 1, 'the tier empty, its marker there');
	$old->stop;
	my $new = PostgreSQL::Test::Cluster->new('history_new_marked');
	$new->init;
	$conf->($new, $tier);
	my ($stdout) = $upgrade->($old, $new, '--copy');
	like($stdout, qr/Upgrade Complete/, 'upgraded');
	$new->start;
	is(scalar(my @n = glob("$tier/databases#*#$em#*")), 0, 'the marker removed');
	$new->safe_psql('postgres', 'DROP DATABASE em');
	pass('and the database dropped');
	$new->stop;
}

# a tier inside the old data directory (its place relative to it): the
# old cluster is not to be deleted, its objects the new cluster's
{
	my $old = PostgreSQL::Test::Cluster->new('history_old_inside');
	$old->init(force_initdb => 1);
	$conf->($old, 'tier');
	$old->start;
	$old->safe_psql('postgres', $fns);
	$old->stop;
	my $new = PostgreSQL::Test::Cluster->new('history_new_inside');
	$new->init(force_initdb => 1);
	$conf->($new, $old->data_dir . '/tier');
	my $bindir = $new->config_data('--bindir');
	unlink 'delete_old_cluster.sh';
	my ($stdout, $stderr) = run_command([
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $bindir,
		'--new-bindir' => $bindir,
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		'--link']);
	like($stdout, qr/Upgrade Complete/, 'a tier inside the old data directory: upgraded');
	like($stdout . $stderr, qr/tiers' objects, now the new cluster's, are inside the old data directory/,
		'saying its objects are inside the old data directory');
	ok(!-e 'delete_old_cluster.sh', 'and making no script to delete the old cluster');
	$new->start;
	is($new->safe_psql('postgres', q{SELECT pg_history_fetch('b1')}), '11', 'its bundle fetched there');
	$new->stop;
}

# refused by --check: a new cluster whose tiers' place is another, or none;
# a relation of a user's in pg_history; and relations not the new cluster's
my $check = sub {
	my ($old, $new) = @_;
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
	return $stdout . $stderr;
};
{
	my $old = PostgreSQL::Test::Cluster->new('history_old_check');
	$old->init;
	$conf->($old, PostgreSQL::Test::Utils::tempdir);
	$old->start;
	$old->safe_psql('postgres', $fns);
	$old->stop;
	my $none = PostgreSQL::Test::Cluster->new('history_new_none');
	$none->init;
	$none->append_conf('postgresql.conf', qq{shared_preload_libraries = 'test_prolly'});
	like($check->($old, $none), qr/the new cluster's settings name no place holding them/,
		'a new cluster naming no place for the tiers\' objects refused');

	# (a store of its own: its identity another's)
	my $other = PostgreSQL::Test::Utils::tempdir;
	append_to_file("$other/store-identity", '0123456789abcdef');
	my $new = PostgreSQL::Test::Cluster->new('history_new_check');
	$new->init;
	$conf->($new, $other);
	like($check->($old, $new), qr/and the new cluster's settings name another place/,
		'one naming another place refused');

	$old->start;
	$old->safe_psql('postgres', q{
		SET allow_system_table_mods = on;
		CREATE TABLE pg_history.mine (a int);
	});
	$old->stop;
	like($check->($old, $new), qr/has a relation in schema pg_history that initdb did not make,\n"?mine/,
		'a relation of a user\'s in pg_history refused');

}
{
	# (the store's relations not the new cluster's)
	my $old = PostgreSQL::Test::Cluster->new('history_old_oids');
	$old->init;
	$conf->($old, PostgreSQL::Test::Utils::tempdir);
	$old->start;
	$old->safe_psql('postgres', q{CREATE EXTENSION test_prolly});
	$old->stop;
	my $new = PostgreSQL::Test::Cluster->new('history_new_oids');
	$new->init;
	$conf->($new, PostgreSQL::Test::Utils::tempdir);
	$new->start;
	# (one of the new cluster's relations named otherwise, its OID the
	# same, by a server allowed to)
	$new->safe_psql('template1', q{
		SET allow_system_table_mods = on;
		ALTER TABLE pg_history.bundles RENAME TO bundles_renamed;
	});
	$new->stop;
	like($check->($old, $new), qr/Database "\w+"'s history store's relations are not the new cluster's/,
		'a store\'s relations not the new cluster\'s refused');
}

# the tier's place: superusers' alone to ask, and asked, not made
{
	my $node = PostgreSQL::Test::Cluster->new('history_place');
	my $tier = PostgreSQL::Test::Utils::tempdir;
	$node->init;
	$conf->($node, $tier);
	$node->start;
	is($node->safe_psql('postgres', q{SELECT binary_upgrade_history_tier_place() IS NULL}), 't',
		'a tier\'s place with no identity made: none');
	ok(!-e "$tier/store-identity", 'and none made');
	$node->safe_psql('postgres', 'CREATE ROLE plain LOGIN');
	my ($ret, $out, $err) = $node->psql('postgres', q{SELECT binary_upgrade_history_tier_place()},
		extra_params => [ '-U', 'plain' ]);
	like($err, qr/permission denied to read the history store's tier's place/, 'not a plain role\'s');
	$node->stop;
}

done_testing();
