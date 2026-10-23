# Copyright (c) 2026, PostgreSQL Global Development Group

# PostGIS on the table AM under test behaves as on heap: its group's
# expected output is heap's.  Needs PostGIS in the installation (the
# Makefile's check-postgis installs it from POSTGIS_BUILD).

use strict;
use warnings FATAL => 'all';
use ProllyTest;
use Test::More;

run_requirements('postgis');
done_testing();
