#-------------------------------------------------------------------------
#
# 095_qvotec_skeleton.pl
#    Stage 2.6 + spec-2.6 v0.2 single-instance QVOTEC aux process surface
#    verification (D19;Sprint A Step 5).
#
#    QVOTEC is the 6th cluster aux process designed to poll voting
#    disks, decide cluster-wide quorum, and broadcast cluster_freeze_
#    writes via the ProcSignal multiplexer.  D8 phase 4 driver QVOTEC
#    spawn integration is DEFERRED to a follow-up hardening round
#    (InitAuxiliaryProcess interaction with shmem inheritance after
#    fork needs dedicated debug); D5/D6/D7/D9 fail-closed primitives
#    are in place as defensive defaults (qvotec_spawn_enabled gate
#    in postmaster.c is false → ServerLoop respawn path bypassed).
#
#    Until D8 lands, this TAP exercises only the catalog / GUC / SRF
#    surface: L1-L11 verify post-initdb visibility of GUCs / views /
#    wait events / counters / fail-closed default state.  Real qvotec
#    spawn-and-poll behavior moves to a future test once D8 ships.
#
#    Test matrix (L1-L11) — catalog/skeleton only:
#
#      L1   cluster.voting_disks GUC registered (postmaster context,
#           default empty)
#      L2   cluster.quorum_poll_interval_ms GUC default 2000ms
#      L3   cluster.voting_disk_io_timeout_ms GUC default 5000ms
#      L4   cluster.voting_disk_size_bytes GUC default 65536 bytes
#      L5   pg_cluster_quorum_state view returns single row;
#           in_quorum=false (Q4 v0.2 fail-closed because qvotec not
#           running per D8 deferral)
#      L6   pg_cluster_voting_disks view returns 0 rows when GUC
#           is empty
#      L7   ClusterBgProcQvotecMainLoop wait event registered
#      L8   ClusterVotingDiskRead + ClusterVotingDiskWrite wait
#           events registered
#      L9   4 cluster.qvotec.* counters present in pg_stat_cluster_
#           counters (poll_cycle / quorum_loss_event / collision_
#           detect_event / disk_io_failure)
#      L10  pg_cluster_quorum_state.collision_state column readable
#           (returns "none" or "(uninitialised)" — both valid pre-spawn)
#      L11  cluster.enabled GUC toggles to off and the cluster
#           subsystem still answers SQL coherently.  Note: this test
#           CANNOT distinguish "qvotec not spawned because GUC=off"
#           from "qvotec not spawned because D8 is deferred" — both
#           paths produce the same in_quorum=false fail-closed
#           default, so L11 is intentionally a vacuous in_quorum
#           assertion paired with a GUC visibility check (real
#           spawn-gate test will land with D8).
#
# IDENTIFICATION
#    src/test/cluster_tap/t/095_qvotec_skeleton.pl
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
use PostgreSQL::Test::Utils;
use Test::More;
use PgracClusterNode;


my $node = PgracClusterNode->new('main');
$node->init;
$node->append_conf('postgresql.conf', "log_min_messages = debug1\n");
$node->start;


# ----------
# L1: cluster.voting_disks GUC registered, default empty.
# ----------
my $vd_default = $node->safe_psql('postgres',
	q{SELECT setting, vartype, context FROM pg_settings WHERE name = 'cluster.voting_disks'});
is($vd_default, '|string|postmaster',
   'L1 cluster.voting_disks GUC registered (string / postmaster / default empty)');


# ----------
# L2: cluster.quorum_poll_interval_ms GUC default 2000ms.
# ----------
my $poll_default = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.quorum_poll_interval_ms'});
is($poll_default, '2000',
   'L2 cluster.quorum_poll_interval_ms default 2000ms (Q4 v0.2 lease window = 2 × 2000)');


# ----------
# L3: cluster.voting_disk_io_timeout_ms GUC default 5000ms.
# ----------
my $io_default = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.voting_disk_io_timeout_ms'});
is($io_default, '5000', 'L3 cluster.voting_disk_io_timeout_ms default 5000ms');


# ----------
# L4: cluster.voting_disk_size_bytes GUC default 65536 bytes.
# ----------
my $size_default = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.voting_disk_size_bytes'});
is($size_default, '65536', 'L4 cluster.voting_disk_size_bytes default 65536 (128 instance slots)');


# ----------
# L5: pg_cluster_quorum_state view returns single row with in_quorum=false.
# Per Q4 v0.2 lease semantics + D8 deferral, qvotec not running →
# in_quorum default false (fail-closed).
# ----------
my $in_quorum = $node->safe_psql('postgres',
	q{SELECT in_quorum FROM pg_cluster_quorum_state});
is($in_quorum, 'f',
   'L5 pg_cluster_quorum_state.in_quorum=false (Q4 v0.2 fail-closed; qvotec not running per D8 deferral)');


# ----------
# L6: pg_cluster_voting_disks 0 rows when cluster.voting_disks is empty.
# ----------
my $vd_count = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_cluster_voting_disks});
is($vd_count, '0',
   'L6 pg_cluster_voting_disks 0 rows when cluster.voting_disks is empty');


# ----------
# L7: ClusterBgProcQvotecMainLoop wait event registered (catalog visible).
# ----------
my $wait_qvotec = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_stat_cluster_wait_events WHERE name = 'ClusterBgProcQvotecMainLoop'});
is($wait_qvotec, '1', 'L7 ClusterBgProcQvotecMainLoop wait event registered');


# ----------
# L8: ClusterVotingDiskRead + ClusterVotingDiskWrite wait events.
# ----------
my $wait_io = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_stat_cluster_wait_events WHERE name IN ('ClusterVotingDiskRead', 'ClusterVotingDiskWrite')});
is($wait_io, '2', 'L8 voting disk I/O wait events (Read + Write) registered');


# ----------
# L9: 4 cluster.qvotec.* atomic counters in pg_stat_cluster_counters.
# ----------
my $counters = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_stat_cluster_counters WHERE name LIKE 'cluster.qvotec.%'});
is($counters, '4',
   'L9 4 cluster.qvotec.* counters (poll_cycle / quorum_loss_event / collision_detect_event / disk_io_failure)');


# ----------
# L10: collision_state column readable; returns "none" or
# "(uninitialised)" — both valid pre-D8.
# ----------
my $collision_state = $node->safe_psql('postgres',
	q{SELECT collision_state FROM pg_cluster_quorum_state});
ok($collision_state eq 'none' || $collision_state eq '(uninitialised)',
   "L10 collision_state column readable (got: $collision_state)");


# ----------
# L11: cluster.enabled=off mode — vacuous in_quorum check + GUC
# visibility check.  CANNOT distinguish "qvotec not spawned because
# cluster.enabled=off" from "qvotec not spawned because D8 deferred";
# both paths produce in_quorum=false.  Pair with a GUC-visible probe
# so at least the GUC toggle round-trips through pg_settings.  Real
# spawn-gate test (qvotec spawn iff cluster.enabled=on) lands when
# D8 ships.
# ----------
$node->stop;
$node->append_conf('postgresql.conf', "cluster.enabled = off\n");
$node->start;

my $cluster_enabled_off = $node->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.enabled'});
is($cluster_enabled_off, 'off',
   'L11a cluster.enabled GUC toggles to off (round-trips through pg_settings)');

my $in_quorum_disabled = $node->safe_psql('postgres',
	q{SELECT in_quorum FROM pg_cluster_quorum_state});
is($in_quorum_disabled, 'f',
   'L11b in_quorum=false in cluster.enabled=off mode (vacuous: same fail-closed default as D8-deferred path; spawn-gate test deferred)');

$node->stop;

done_testing();
