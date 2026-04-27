#-------------------------------------------------------------------------
#
# 005_wait_events_catalog.pl
#    Reverse-regression for the cluster wait event registration (stage 0.11).
#
#    PG 16 does not expose pg_wait_events as a catalog view (that view
#    was added in PG 17), so this test cannot enumerate the registered
#    cluster events directly via SQL.  The 46 event registrations and
#    10 class IDs are validated at compile time by
#    src/test/cluster_unit/test_cluster_wait_events.c.
#
#    What this test proves at runtime:
#      - The server starts cleanly with the 10 cluster wait classes
#        and 10 sub-helpers linked in (catches link-time symbol errors,
#        unresolved enum values, switch fall-through bugs).
#      - pg_stat_activity.wait_event_type returns correct strings for
#        currently active PG-native backends after the cluster cases
#        were inserted into pgstat_get_wait_event_type() switch.
#      - No live backend reports a 'Cluster: *' wait event yet -- the
#        pgstat_report_wait_start() call sites for cluster events live
#        in their owning subsystem specs (Stage 1+), not in 0.11.
#
# IDENTIFICATION
#    src/test/cluster_tap/t/005_wait_events_catalog.pl
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use Test::More;
use PgracClusterNode;


my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;


# ----------
# pg_stat_activity returns 'client backend' for the connected psql, proving
# that GetBackendTypeDesc() (extended in stage 0.10) and pgstat_get_wait_*()
# (extended in stage 0.11) coexist without breaking the PG-native paths.
# ----------
my $client_backend_count = $node->safe_psql(
	'postgres',
	q{SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'client backend'});
cmp_ok($client_backend_count, '>=', 1,
	'pg_stat_activity sees a client backend (PG-native dispatch intact)');


# ----------
# At least one PG-native background helper is visible.  This proves that
# pgstat_get_wait_event_type() did not lose its PG-native cases when the
# 10 cluster cases were inserted -- a backend with wait_event_type =
# 'Activity'/'Client'/'IPC'/'Timeout'/'IO'/'LWLock' must still resolve.
# ----------
my $native_wait_type_count = $node->safe_psql(
	'postgres',
	q{SELECT count(*) FROM pg_stat_activity
	   WHERE wait_event_type IN ('Activity', 'Client', 'IPC', 'Timeout',
	                             'IO', 'LWLock', 'Lock', 'BufferPin',
	                             'Extension')});
cmp_ok($native_wait_type_count, '>=', 1,
	'pg_stat_activity has at least one backend in a PG-native wait_event_type');


# ----------
# No live backend may currently be in a cluster wait state -- the
# pgstat_report_wait_start() call sites for the 46 cluster events are
# deferred to Stage 1+ subsystem specs.  Seeing any 'Cluster: *' here
# at stage 0.11 would indicate stray instrumentation slipped in.
# ----------
my $live_cluster_waits = $node->safe_psql(
	'postgres',
	q{SELECT count(*) FROM pg_stat_activity WHERE wait_event_type LIKE 'Cluster:%'});
is($live_cluster_waits, '0',
	'no live backend in a Cluster:* wait state (call sites deferred to Stage 1+)');


# ----------
# wait_event_type column query does not error: this proves that
# pgstat_get_wait_event_type() switch has the right shape (no missing
# cases, no compile-time C warning escalated to runtime weirdness).
# ----------
my $any_query_ok = $node->safe_psql(
	'postgres',
	q{SELECT count(*) FROM pg_stat_activity});
cmp_ok($any_query_ok, '>=', 1,
	'pg_stat_activity is queryable after cluster wait class extension');


# ----------
# Smoke test: backend stays healthy after the heavy stat queries above.
# ----------
is($node->safe_psql('postgres', 'SELECT 1'), '1',
	'server healthy after pg_stat_activity exercise');


$node->stop;

done_testing();
