# Copyright (c) 2026, PostgreSQL Global Development Group
#
# What an object says of a column of its schema's tables it says by the column's tag, so it
# follows a column renamed on another branch, with no change to its text.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('tags');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";

my $objects = q{
	CREATE VIEW v AS SELECT val, n + 1 AS p FROM t WHERE val IN ('a', 'b');
	CREATE VIEW aliased AS SELECT a.val AS first, b.val AS second FROM t a JOIN t b ON a.n = b.n;
	CREATE VIEW sub AS SELECT x.val FROM (SELECT val FROM t) x;
	CREATE VIEW listed AS SELECT l FROM t x(l, m);
	CREATE VIEW joined AS SELECT a.val, b.other FROM t a JOIN u b USING (n);
	CREATE MATERIALIZED VIEW mv AS SELECT val FROM t WITH NO DATA;
	CREATE INDEX mv_val ON mv (val);
	CREATE FUNCTION trf() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
	CREATE TRIGGER tg BEFORE UPDATE OF val, n ON t FOR EACH ROW WHEN (NEW.val <> 'x') EXECUTE FUNCTION trf();
	CREATE RULE ru AS ON DELETE TO t WHERE OLD.val = 'a' DO ALSO DELETE FROM u WHERE other = OLD.val;
	CREATE POLICY po ON t USING (val <> 'x') WITH CHECK (n > 0);
	CREATE FUNCTION body() RETURNS SETOF varchar LANGUAGE sql BEGIN ATOMIC SELECT val FROM t; END;
	CREATE FUNCTION writes(p varchar) RETURNS void LANGUAGE sql BEGIN ATOMIC INSERT INTO t (val, n) VALUES (p, 1); UPDATE t SET val = p WHERE n = 2; END;
	CREATE FUNCTION text_body() RETURNS bigint LANGUAGE sql AS 'SELECT count(*) FROM t';
	CREATE STATISTICS st ON val, n FROM t;
	CREATE SEQUENCE sq OWNED BY t.n;
};
$node->safe_psql('postgres', qq{CREATE ROLE g_tags NOLOGIN; CREATE SCHEMA src VERSIONED; ALTER SCHEMA src OWNER TO g_tags; CREATE SCHEMA dst VERSIONED; ALTER SCHEMA dst OWNER TO g_tags;
	SET ROLE g_tags; SET search_path = src;
	CREATE TABLE t (val varchar(10), n integer); CREATE TABLE u (n integer, other varchar(10));
	INSERT INTO t VALUES ('a', 1); INSERT INTO u VALUES (1, 'a');
	$objects
});

my $held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('src')});
my %was = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $held;
like($was{'view/v'}, qr/\@\{\d+\} AS val/, 'a view says its column by its tag, and keeps its own column\'s name');
like($was{'view/aliased'}, qr/a\.\@\{\d+\} AS first/, 'under an alias of its table');
like($was{'view/sub'}, qr/t\.\@\{\d+\} AS val/, 'in a subquery');
like($was{'view/sub'}, qr/SELECT val\b/, 'but not a subquery\'s own column');
like($was{'view/listed'}, qr/SELECT l\b/, 'and not a column an alias list has named');
like($was{'trigger/tg(t)'}, qr/UPDATE OF \@\{\d+\}, \@\{\d+\}/, 'a trigger\'s columns');
like($was{'trigger/tg(t)'}, qr/new\.\@\{\d+\}/, 'and its WHEN');
like($was{'rule/ru(t)'}, qr/old\.\@\{\d+\}/, 'a rule');
like($was{'policy/po(t)'}, qr/\@\{\d+\}/, 'a policy');
like($was{'function/body()'}, qr/\@\{\d+\}/, 'an atomic body');
like($was{'function/writes(character varying)'}, qr/INSERT INTO t \(\@\{\d+\}, \@\{\d+\}\)/, 'an INSERT\'s list of columns');
like($was{'function/writes(character varying)'}, qr/SET \@\{\d+\} = /, 'an UPDATE\'s');
like($was{'statistics/st'}, qr/ON \@\{\d+\}, \@\{\d+\} FROM t/, 'a statistics object\'s');
like($was{'sequence/sq'}, qr/\@\{\d+\}/, 'and what a sequence is owned by');
like($was{'function/text_body()'}, qr/SELECT count\(\*\) FROM t/, 'a text body is left as it is');
like($was{'view/joined'}, qr/JOIN u b USING \(n\)/, 'as is a USING\'s name');

# a rename changes no object's text
$node->safe_psql('postgres', q{SET ROLE g_tags; ALTER TABLE src.t RENAME COLUMN val TO val2; ALTER TABLE src.t RENAME COLUMN n TO n2; ALTER TABLE src.u RENAME COLUMN other TO other2});
my %renamed = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('src')});
my @moved = grep { ($renamed{$_} // '') ne $was{$_} } sort keys %was;
is(join(', ', @moved), 'view/joined', 'a rename changes the text of nothing, but what says a USING');
$node->safe_psql('postgres', q{SET ROLE g_tags; ALTER TABLE src.t RENAME COLUMN val2 TO val; ALTER TABLE src.t RENAME COLUMN n2 TO n; ALTER TABLE src.u RENAME COLUMN other2 TO other});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('src')}), $held, 'and renamed back it is as it was');

# restored where the column has another name, it means that column
for my $table ('t', 'u')
{
	my $hex = $node->safe_psql('postgres', qq{SELECT encode(test_ws_schema_bytes('src', '$table'), 'hex')});
	$node->safe_psql('postgres', qq{SET ROLE g_tags; SELECT test_ws_schema_create(decode('$hex', 'hex'), 'dst'); SELECT test_ws_schema_set_tags(decode('$hex', 'hex'), 'dst')});
}
$node->safe_psql('postgres', q{SET ROLE g_tags; ALTER TABLE dst.t RENAME COLUMN val TO renamed; INSERT INTO dst.t VALUES ('a', 1); INSERT INTO dst.u VALUES (1, 'a')});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('src', 'dst', 'both', true)});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('dst')}), $held, 'restored where a column has another name, it reads as its commit');
is($node->safe_psql('postgres', q{SELECT val, p FROM dst.v}), 'a|2', 'and returns the same, under its own columns\' names');
like($node->safe_psql('postgres', q{SELECT pg_get_viewdef('dst.v'::regclass)}), qr/renamed/, 'of the column of that name');
is($node->safe_psql('postgres', q{SELECT tgattr::text FROM pg_trigger WHERE tgname = 'tg' AND tgrelid = 'dst.t'::regclass}), '1 2', 'a trigger on the same columns');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_statistic_ext WHERE stxname = 'st' AND stxrelid = 'dst.t'::regclass}), '1', 'a statistics object made');
is($node->safe_psql('postgres', q{SELECT pg_get_serial_sequence('dst.t', 'n')}), 'dst.sq', 'a sequence owned by its column');

# the same rename where one was restored and one was not holds the same text
$node->safe_psql('postgres', q{SET ROLE g_tags; ALTER TABLE src.t RENAME COLUMN val TO renamed});
my %src_after = map { my ($k, $v) = split /\t/; $k => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('src')});
my %dst_after = map { my ($k, $v) = split /\t/; $k => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('dst')});
is(join(', ', grep { ($src_after{$_} // '') ne ($dst_after{$_} // '') } sort keys %src_after), '', 'the same rename, on what was restored and what was not, holds the same text');
$node->safe_psql('postgres', q{SET ROLE g_tags; ALTER TABLE src.t RENAME COLUMN renamed TO val});

# what prints otherwise once restored is still remembered, by what it says with its tags
isnt($node->safe_psql('postgres', q{SELECT count(*) FROM pg_ws_print WHERE prtnamespace = 'dst'::regnamespace AND prtkey = 'view/v'}), '0', 'an IN on a varchar is remembered');
like($node->safe_psql('postgres', q{SELECT prtheld FROM pg_ws_print WHERE prtnamespace = 'dst'::regnamespace AND prtkey = 'view/v'}), qr/\@\{\d+\}/, 'with its tags');

# an @ of what is printed stays an @
$node->safe_psql('postgres', q{CREATE SCHEMA at_s VERSIONED; ALTER SCHEMA at_s OWNER TO g_tags; SET ROLE g_tags; CREATE TABLE at_s.t (a text);
	CREATE VIEW at_s.w AS SELECT a, 'x@y' AS b FROM at_s.t WHERE a = '@{1}'});
like(pack('H*', (split /\t/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('at_s')}))[1]), qr/x\@\@y.*\@\@\{1\}/s, 'a string with an @ is no reference');
$node->safe_psql('postgres', q{DROP SCHEMA at_s CASCADE});

# the same tag on two columns of two names is refused, if it is referred to
$node->safe_psql('postgres', q{CREATE SCHEMA amb VERSIONED; ALTER SCHEMA amb OWNER TO g_tags; SET ROLE g_tags; CREATE TABLE amb.t (a integer);
	ALTER TABLE amb.t RENAME TO t_old; CREATE TABLE amb.t (a integer); ALTER TABLE amb.t_old RENAME COLUMN a TO a2;
	CREATE VIEW amb.un AS SELECT 1 AS k});
unlike($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('amb')}), qr/\@\{/, 'nothing refers to it yet, so it is held');
$node->safe_psql('postgres', q{SET ROLE g_tags; CREATE VIEW amb.w AS SELECT a FROM amb.t});
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('amb')});
like($err, qr/whose tag another of another name has/, 'what refers to a tag two columns of two names have is refused');
$node->safe_psql('postgres', q{SET ROLE g_tags; ALTER TABLE amb.t_old RENAME COLUMN a2 TO a});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('amb')}), qr/^view\/w\t/m, 'of one name it is held');
$node->safe_psql('postgres', q{DROP SCHEMA amb CASCADE});

$node->stop;
done_testing();
