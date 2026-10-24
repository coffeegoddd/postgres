# Copyright (c) 2026, PostgreSQL Global Development Group

# PostGIS under concurrency on the table AM under test behaves as on heap:
# spatial predicate locks, an exclusion constraint race and row locks on
# KNN results (postgis_isolation_schedule; expected output is heap's).
# Needs PostGIS in the installation (the Makefile's check-postgis installs
# it from POSTGIS_BUILD).

use strict;
use warnings FATAL => 'all';
use ProllyTest;
use Test::More;

run_requirements('postgis_isolation');
done_testing();
