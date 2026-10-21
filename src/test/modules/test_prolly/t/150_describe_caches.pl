# Copyright (c) 2026, PostgreSQL Global Development Group

# What a fold caches in the backend (#483), changed by another session:
# its invalidations reach the cache before the next fold, which is the
# same as a fresh session's.  The trigger's function renamed sends its
# table nothing; ALTER EXTENSION ADD and DEPENDS ON EXTENSION change only
# pg_depend, which has no syscache.  The cache's own check is off in the
# session that takes from it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('describe_caches');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION test_prolly_b;
	SET dolt.author = 'Ada Lovelace <ada@example.com>';
	CREATE SCHEMA s VERSIONED;
	CREATE FUNCTION s.touch() RETURNS trigger LANGUAGE plpgsql AS
	  $$BEGIN RETURN NEW; END$$;
	CREATE TYPE public.hue AS ENUM ('red', 'blue');
	CREATE TABLE s.t (id int PRIMARY KEY, v text, c public.hue);
	CREATE TRIGGER t_touch BEFORE UPDATE ON s.t FOR EACH ROW
	  EXECUTE FUNCTION s.touch();
	CREATE FUNCTION s.one() RETURNS int LANGUAGE sql AS 'SELECT 1';
	INSERT INTO s.t VALUES (1, 'a', 'red');
	SELECT dolt_commit('-Am', 'base');
});

my $a = $node->background_psql('postgres');
$a->query_safe(q{SET debug_prolly_verify_fold_caches = off});
$a->query_safe(q{SET dolt.author = 'Ada Lovelace <ada@example.com>'});

# The session's working set folded from its baseline (its caches warm
# from the fold before), and a fresh session's folded in full.
sub same_as_fresh
{
	my $warm = $a->query_safe(q{SELECT test_fold_root()});
	return $warm eq sql(q{SELECT test_fold_root(true)});
}

$a->query_safe(q{UPDATE s.t SET v = 'b'});
$a->query_safe(q{SELECT count(*) FROM dolt_status});
sql(q{ALTER FUNCTION s.touch() RENAME TO touched});
ok(same_as_fresh(), 'a trigger function renamed elsewhere');

sql(q{ALTER TABLE s.t ADD COLUMN w int DEFAULT 3});
ok(same_as_fresh(), 'a column added elsewhere');

# The dependencies a commit records: the warm session's, then a fresh
# session's of the same working set.
sub deps
{
	my ($s) = @_;
	return $s->query_safe(
		q{SELECT array_to_string(test_commit_dependencies(dolt_hashof('HEAD')), ';')});
}

$a->query_safe(q{SELECT dolt_commit('-Am', 'warm')});
my $before = deps($a);
like($before, qr/type public\.hue/, 'an ordinary schema\'s type');
sql(q{ALTER EXTENSION test_prolly_b ADD TYPE public.hue});
$a->query_safe(q{SELECT dolt_commit('--allow-empty', '-Am', 'member')});
like(deps($a), qr/^extension test_prolly_b[^;]*$/,
	'the type an extension\'s member, by ALTER EXTENSION ADD elsewhere');
sql(q{ALTER EXTENSION test_prolly_b DROP TYPE public.hue});
sql(q{ALTER FUNCTION s.one() DEPENDS ON EXTENSION test_prolly_b});
$a->query_safe(q{SELECT dolt_commit('--allow-empty', '-Am', 'depends')});
like(deps($a), qr/extension test_prolly_b.*;type public\.hue/,
	'DEPENDS ON EXTENSION elsewhere');

$a->quit;
$node->stop;
done_testing();
