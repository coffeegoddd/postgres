# Copyright (c) 2026, PostgreSQL Global Development Group

# An object written is there whole after a crash, or not at all: one
# written in full survives an immediate stop; one whose bytes were written
# and synced but not yet linked into place is not there after the crash,
# its temporary file passed by in listings, and the name may be written.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('object_store_crash');
$node->init;
$node->start;
$node->safe_psql('postgres', q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;});
my $loc = 'pg_test_objects';

# written in full, then an immediate stop: there
$node->safe_psql('postgres', qq{SELECT test_os_put('directory', '$loc', 'p/kept', '\\x0a0b0c')});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', qq{SELECT test_os_read('directory', '$loc', 'p/kept', 0, 3)}),
	'\x0a0b0c', 'an object written survives a crash');

# its bytes written and synced, the crash before it is linked into place
my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe(q{SELECT injection_points_set_local()});
$s->query_safe(q{SELECT injection_points_attach('object-store-written', 'wait')});
$s->query_until(qr/writing/,
	"\\echo writing\nSELECT test_os_put('directory', '$loc', 'p/cut', '\\x01020304');\n");
$node->wait_for_event('client backend', 'object-store-written');
$node->stop('immediate');
$s->{run}->finish;
$node->start;
is($node->safe_psql('postgres', qq{SELECT test_os_size('directory', '$loc', 'p/cut') IS NULL}),
	't', 'one cut short is not there');
ok(grep(/\.tmp\./, glob($node->data_dir . "/$loc/p/*")), 'its temporary file left');
is($node->safe_psql('postgres', qq{SELECT string_agg(name, ',' ORDER BY name) FROM test_os_list('directory', '$loc', '')}),
	'p/kept', 'listings pass it by');
$node->safe_psql('postgres', qq{SELECT test_os_put('directory', '$loc', 'p/cut', '\\x05')});
is($node->safe_psql('postgres', qq{SELECT test_os_read('directory', '$loc', 'p/cut', 0, 1)}),
	'\x05', 'and the name may be written');

$node->stop;
done_testing();
