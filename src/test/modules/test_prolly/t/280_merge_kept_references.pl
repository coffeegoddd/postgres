# Copyright (c) 2026, PostgreSQL Global Development Group

# A merge of a row both sides changed compares a long value by its
# reference before reading it: where a side's reference is base's, that
# side didn't change the column, so a large value neither side changed is
# neither fetched (text, bytea: hash pointers) nor decoded (jsonb, read as
# it is decoded), and the merged row holds history's reference.  A long
# value either side changed is still compared by value, so changes alike
# merge, and different changes (of the same length, too) conflict.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('kept');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA a VERSIONED;
	CREATE TABLE a.docs (id int PRIMARY KEY, status int, owner int,
		t text, b bytea, j jsonb);
	INSERT INTO a.docs
	SELECT g, 0, 0, string_agg(md5(g || '/' || i), '' ORDER BY i),
		   decode(string_agg(md5(g || '|' || i), '' ORDER BY i), 'hex'),
		   jsonb_build_object('g', g, 'parts', jsonb_agg(md5(g || '-' || i)))
	FROM generate_series(1, 4) g, generate_series(1, 3000) i GROUP BY g;
	SELECT length(dolt_commit('-Am', 'docs')::text);
	SELECT dolt_branch('other');
});

my $roots = q{SELECT string_agg(id || ':' || (test_toast_pointer(t)).root
	|| ':' || (test_toast_pointer(b)).root, ',' ORDER BY id) FROM a.docs};
my $docs = sql($roots);
my $digest = q{SELECT string_agg(id || ':' || status || ':' || owner || ':'
	|| md5(t) || ':' || md5(b) || ':' || md5(j::text), ',' ORDER BY id)
	FROM a.docs};
my $before = sql($digest);

# Ours changes status, theirs owner, of every row: each row is merged
# column by column.
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT dolt_checkout('other')});
$s->query_safe(q{UPDATE a.docs SET owner = 7});
$s->query_safe(q{SELECT length(dolt_commit('-am', 'owners')::text)});
$s->quit;
sql(q{
	UPDATE a.docs SET status = 1;
	SELECT length(dolt_commit('-am', 'statuses')::text);
});

# With each read of a value tree by a decoder noticed, and the value
# trees read by hash pointers counted.
my ($ret, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-value-read', 'notice');
	SELECT count(*) FROM test_toast_counters(true);
	SELECT FROM dolt_merge('other');
	SELECT nodes_read FROM test_toast_counters();
});
is($ret, 0, 'the merge succeeds') or diag($err);
unlike($err, qr/prolly-value-read/, 'it reads no jsonb value');
is((split /\n/, $out)[-1], '0', 'and fetches no text or bytea value');
is(sql(q{SELECT string_agg(status || ':' || owner, ',' ORDER BY id) FROM a.docs}),
	'1:7,1:7,1:7,1:7', 'both sides\' changes are merged');
is(sql($roots), $docs, 'the merged rows hold history\'s trees');
(my $want = $before) =~ s/:0:0:/:1:7:/g;
is(sql($digest), $want, 'and the values read back');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'the merge committed');

# Long values the sides changed: row 1 ours alone, row 2 theirs alone,
# row 3 both alike, row 4 both differently, to values of the same length.
sql(q{SELECT dolt_branch('-f', 'other', 'HEAD')});
$s = $node->background_psql('postgres');
$s->query_safe(q{SELECT dolt_checkout('other')});
$s->query_safe(q{UPDATE a.docs SET owner = 8});
$s->query_safe(q{UPDATE a.docs SET t = 'T' || substr(t, 2), j = j || '{"x": 2}'
	WHERE id IN (2, 3)});
$s->query_safe(q{UPDATE a.docs SET b = '\x01'::bytea || substr(b, 2) WHERE id = 4});
$s->query_safe(q{SELECT length(dolt_commit('-am', 'theirs')::text)});
$s->quit;
sql(q{
	UPDATE a.docs SET status = 2;
	UPDATE a.docs SET t = 'O' || substr(t, 2), j = j || '{"x": 1}' WHERE id = 1;
	UPDATE a.docs SET t = 'T' || substr(t, 2), j = j || '{"x": 2}' WHERE id = 3;
	UPDATE a.docs SET b = '\x02'::bytea || substr(b, 2) WHERE id = 4;
	SELECT length(dolt_commit('-am', 'ours')::text);
});
my $t4 = sql(q{SELECT left(t, 1) FROM a.docs WHERE id = 4});
# (in a transaction, to see the conflict; row 4 keeps ours)
my @got = split /\n/, sql(q{
	BEGIN;
	SELECT FROM dolt_merge('other');
	SELECT string_agg(num_conflicts::text, ',') FROM dolt_conflicts;
	SELECT string_agg(id || ':' || status || ':' || owner || ':'
		|| left(t, 1) || ':' || coalesce(j->>'x', '-') || ':'
		|| CASE WHEN id = 4 THEN encode(substr(b, 1, 1), 'hex') ELSE '' END,
		',' ORDER BY id) FROM a.docs;
	ROLLBACK;
});
is($got[-2], '1', 'different changes of a long value conflict');
is($got[-1], "1:2:8:O:1:,2:2:8:T:2:,3:2:8:T:2:,4:2:7:$t4:-:02",
	'a value one side changed is taken, one both changed alike kept');

$node->stop;
done_testing();
