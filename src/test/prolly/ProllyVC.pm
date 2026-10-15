# Copyright (c) 2026, PostgreSQL Global Development Group

package ProllyVC;

use strict;
use warnings FATAL => 'all';
use Exporter 'import';
use Digest::SHA qw(sha256_hex);

our @EXPORT = qw(check_vc_target);

# Run only in a disposable database initialized with ProllyTest::setup_target.
# Throw on the first failed operation or invariant, including its stage.  Do
# not emit TAP assertions here: negative controls must exercise the same probe.
# Every call opens a new connection, so all writes below are SQL-committed
# before a Dolt commit snapshots them.  Checkout is session-local.
sub check_vc_target
{
	my ($node, $db, $am) = @_;
	my $checks = 0;
	my $sql = sub {
		my ($stage, $branch, $query) = @_;
		my $checkout = $branch eq 'main' ? ''
		  : "DO \$\$ BEGIN PERFORM dolt_checkout('$branch'); END \$\$;";
		my ($rc, $out, $err) = $node->psql($db, "$checkout $query");
		die "VC target $am [$stage]: SQL failed: $err\n" if $rc;
		return $out;
	};
	my $equal = sub {
		my ($stage, $got, $want) = @_;
		if ($got ne $want)
		{
			my $describe = sub {
				my ($value) = @_;
				return $value if length($value) <= 200;
				return substr($value, 0, 100) . '... (' . length($value)
				  . ' bytes, sha256=' . sha256_hex($value) . ')';
			};
			die "VC target $am [$stage]: expected <" . $describe->($want)
			  . '>, got <' . $describe->($got) . ">\n";
		}
		$checks++;
	};
	my $read = sub {
		my ($stage, $branch, $want) = @_;
		$equal->("$stage branch", $sql->($stage, $branch,
			'SELECT active_branch()'), $branch);
		$equal->("$stage AM", $sql->($stage, $branch, q{
			SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
			WHERE c.oid = 'vc_canary'::regclass}), $am);
		# Compare the full values, including a value larger than a heap page.
		# These small fixtures have explicit ordering and no NULL encoding.
		$equal->("$stage rows", $sql->($stage, $branch,
			'SELECT id, payload FROM vc_canary ORDER BY id'), $want);
	};
	my $commit = sub {
		my ($stage, $branch, $parent) = @_;
		my $id = $sql->($stage, $branch,
			"SELECT (dolt_commit('-m', '$stage'))[1]");
		die "VC target $am [$stage]: empty commit identifier\n" if $id eq '';
		$equal->("$stage HEAD", $sql->($stage, $branch,
			'SELECT dolt_hashof(\'HEAD\')'), $id);
		if (defined $parent)
		{
			die "VC target $am [$stage]: commit did not advance HEAD\n"
			  if $id eq $parent;
			$equal->("$stage parent", $sql->($stage, $branch,
				"SELECT dolt_hashof('HEAD^')"), $parent);
		}
		return $id;
	};

	# Incompressible, deterministic text: large enough for TOAST on prolly.
	my $large = join('', map { sprintf('%08x', $_ * 2654435761 % 4294967296) }
		  1 .. 2048);
	my $base_rows = "1|base\n2|delete-me\n3|$large";
	my $feature_rows = "1|feature\n3|$large\n4|feature-only";
	my $main_rows = "1|main\n2|delete-me\n3|$large\n5|main-only";
	$sql->('fixture', 'main', qq{
		CREATE TABLE vc_canary (id int PRIMARY KEY, payload text);
		INSERT INTO vc_canary VALUES (1, 'base'), (2, 'delete-me'), (3, '$large');
	});
	$read->('initial', 'main', $base_rows);
	my $base = $commit->('base commit', 'main', undef);
	$sql->('create branch', 'main', "SELECT dolt_branch('vc_feature')");
	$equal->('branch starts at base', $sql->('branch head', 'vc_feature',
		"SELECT dolt_hashof('HEAD')"), $base);
	$read->('branch copy', 'vc_feature', $base_rows);

	$sql->('feature edits', 'vc_feature', q{
		UPDATE vc_canary SET payload = 'feature' WHERE id = 1;
		DELETE FROM vc_canary WHERE id = 2;
		INSERT INTO vc_canary VALUES (4, 'feature-only');
	});
	$read->('main isolated from feature', 'main', $base_rows);
	$read->('feature working set', 'vc_feature', $feature_rows);
	my $feature = $commit->('feature commit', 'vc_feature', $base);

	$sql->('main edits', 'main', q{
		UPDATE vc_canary SET payload = 'main' WHERE id = 1;
		INSERT INTO vc_canary VALUES (5, 'main-only');
	});
	my $main = $commit->('main commit', 'main', $base);
	$read->('feature isolated from main', 'vc_feature', $feature_rows);
	$read->('main working set', 'main', $main_rows);

	# A returned commit id is insufficient: restore its data after changing
	# every row, including the large value.  Then restore an older commit.
	$sql->('dirty feature', 'vc_feature', q{
		UPDATE vc_canary SET payload = 'uncommitted-to-history';
		INSERT INTO vc_canary VALUES (6, 'discard-me');
	});
	$sql->('restore feature HEAD', 'vc_feature', "SELECT dolt_reset('--hard')");
	$read->('restored feature HEAD', 'vc_feature', $feature_rows);
	$equal->('reset keeps feature HEAD', $sql->('feature HEAD', 'vc_feature',
		"SELECT dolt_hashof('HEAD')"), $feature);
	(my $quoted_base = $base) =~ s/'/''/g;
	$sql->('restore base', 'vc_feature', "SELECT dolt_reset('--hard', '$quoted_base')");
	$read->('restored base', 'vc_feature', $base_rows);
	$equal->('reset moves feature HEAD', $sql->('base HEAD', 'vc_feature',
		"SELECT dolt_hashof('HEAD')"), $base);
	$read->('main survives feature reset', 'main', $main_rows);
	$equal->('main HEAD survives feature reset', $sql->('main HEAD', 'main',
		"SELECT dolt_hashof('HEAD')"), $main);
	return $checks;
}

1;
