# Copyright (c) 2026, PostgreSQL Global Development Group

# A table rewrite (ALTER TABLE ... TYPE) checkpoints the session's branch
# by folding it.  That fold covers every table of the branch, so an error
# a table other than the rewritten one raises in it isn't the rewrite's:
# here a stored aclitem whose role was dropped, whose text form no longer
# reads back (the error a fold raises is 42704).  The ALTER succeeds, the
# skipped checkpoint is logged with the error's SQLSTATE and counted, the
# next commit reports the error itself, and once the row is fixed folds
# what the checkpoint didn't, as a full fold would.  With the pages change
# finder the error comes in the middle of the fold, after it has begun on
# the table's pages.  A cancel, and an internal error (class XX, here an
# injection point's), still fail the ALTER.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('rewrite_checkpoint');
$node->init;
$node->append_conf('postgresql.conf', qq{autovacuum = off
log_error_verbosity = verbose});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub skipped
{
	return sql(q{SELECT value FROM pg_stat_prolly
		WHERE name = 'rewrite_checkpoints_skipped'});
}

sql(q{CREATE EXTENSION injection_points; CREATE EXTENSION test_prolly; CREATE ROLE keeper});

foreach my $finder ('', 'pages')
{
	my $s = $finder eq '' ? 'logged' : 'paged';
	sql(qq{ALTER SYSTEM SET debug_prolly_change_finder = '$finder'; SELECT pg_reload_conf()});
	sql(qq{CREATE ROLE ${s}_gone;
		CREATE SCHEMA $s VERSIONED;
		CREATE TABLE $s.t (id int PRIMARY KEY, v text);
		CREATE TABLE $s.acl (id int PRIMARY KEY, x aclitem);
		CREATE TABLE $s.other (id int PRIMARY KEY, v text);
		INSERT INTO $s.t SELECT i, 'v' || i FROM generate_series(1, 1000) i;
		INSERT INTO $s.other SELECT i, 'o' || i FROM generate_series(1, 100) i;
		INSERT INTO $s.acl SELECT i, 'keeper=r/keeper' FROM generate_series(1, 200) i;
		SELECT dolt_commit('-Am', '$s base')});
	sql(qq{UPDATE $s.other SET v = v || '!' WHERE id <= 10;
		INSERT INTO $s.acl VALUES (1000, '${s}_gone=r/keeper');
		UPDATE $s.acl SET x = 'keeper=w/keeper' WHERE id <= 5;
		DROP ROLE ${s}_gone});

	my $before = skipped();
	my $offset = -s $node->logfile;
	my ($rc, $out, $err) = $node->psql('postgres', qq{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-fold-begun', 'notice');
		ALTER TABLE $s.t ALTER COLUMN id TYPE bigint;
	});
	is($rc, 0, "$s: a rewrite whose checkpoint fails on another table succeeds");
	if ($finder eq 'pages')
	{
		like($err, qr/notice triggered for injection point prolly-fold-begun/,
			"$s: the checkpoint's fold had begun on the pages");
	}
	unlike($err, qr/ERROR|not checkpointed/, "$s: the client sees no error");
	my $log = slurp_file($node->logfile, $offset);
	like($log, qr/LOG:  42704: branch not checkpointed after rewriting $s\.t: role "\d+" does not exist/,
		"$s: the skipped checkpoint is logged, with the error's SQLSTATE");
	# (the ALTER's backend reports its counts as it exits)
	ok($node->poll_query_until('postgres', qq{SELECT value = $before + 1 FROM pg_stat_prolly
		WHERE name = 'rewrite_checkpoints_skipped'}), "$s: and counted");
	is(sql(qq{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
		WHERE attrelid = '$s.t'::regclass AND attname = 'id'}),
		'bigint', "$s: the column was retyped");

	($rc, $out, $err) = $node->psql('postgres', qq{SELECT dolt_commit('-am', '$s retyped')});
	like($err, qr/role "\d+" does not exist/, "$s: the next commit reports the error itself");
	sql(qq{DELETE FROM $s.acl WHERE id = 1000});
	is(sql(q{SELECT test_fold_root(false) = test_fold_root(true)}), 't',
		"$s: the next fold is a full fold's");
	sql(qq{SELECT dolt_commit('-am', '$s retyped')});
	is(sql(qq{SELECT
		(SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::$s.other)) || ' ' ||
		(SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', NULL::$s.acl))}),
		'10 5', "$s: and the commit has every change");
	is(sql(qq{SELECT count(*), sum(id) FROM $s.t}), '1000|500500', "$s: and the rewritten rows");
}

# An internal error in the checkpoint (an injection point's, XX000) isn't
# set aside: the ALTER fails, and the column keeps its type.
my ($rc, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-fold-stamp', 'error');
	ALTER TABLE paged.t ALTER COLUMN v TYPE varchar(200);
});
like($err, qr/error triggered for injection point prolly-fold-stamp/,
	'an internal error in the checkpoint fails the rewrite');
is(sql(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
	WHERE attrelid = 'paged.t'::regclass AND attname = 'v'}),
	'text', 'and the column keeps its type');

# A cancel in the checkpoint cancels the ALTER: it waits at the same point
# and is cancelled there.
my $alter = $node->background_psql('postgres', on_error_stop => 0);
$alter->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-fold-stamp', 'wait')});
$alter->query_until(qr/started/, q{
	\echo started
	ALTER TABLE paged.t ALTER COLUMN v TYPE varchar(200) USING v || '';
	\echo done
});
$node->wait_for_event('client backend', 'prolly-fold-stamp');
sql(q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'prolly-fold-stamp'});
$alter->query_until(qr/done/, '');
like($alter->{stderr}, qr/canceling statement due to user request/,
	'a cancel in the checkpoint cancels the rewrite');
$alter->quit;
is(sql(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute
	WHERE attrelid = 'paged.t'::regclass AND attname = 'v'}),
	'text', 'and the column keeps its type');

$node->stop;
done_testing();
