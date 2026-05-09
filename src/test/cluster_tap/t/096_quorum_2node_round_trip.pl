#-------------------------------------------------------------------------
#
# 096_quorum_2node_round_trip.pl
#    Stage 2.6 + spec-2.6 v0.2 D20 — 2-node ClusterPair round-trip TAP
#    placeholder.
#
#    Per spec-2.6 D20, this TAP would exercise full L1-L8 2-node
#    qvotec round-trip:
#      L1   ClusterPair start_pair both alive + voting disks share
#      L2   both nodes register self in voting disks
#      L3   quorum_view shows 2 alive
#      L4   SIGSTOP one node → other detects quorum loss within 5s
#      L5   SIGCONT → recovery
#      L6   collision check: 2 instances configured with same node_id →
#           one FATAL via collision detect (Q6 v0.2 newer-self-FATAL)
#      L7   boot-time epoch recovery: kill all, restart, last_known_
#           epoch + 1
#      L8   storage stub mode all-disk-fail → both fail-closed
#
#    All eight scenarios depend on the QVOTEC aux process actually
#    spawning, polling voting disks, and broadcasting cluster_freeze_
#    writes via ProcSignal — i.e., they require Sprint A Step 3 D8
#    (phase 4 driver QVOTEC spawn integration), which is currently
#    DEFERRED.  See spec-2.6 Sprint A Step 3 deferral note +
#    cluster_startup_phase.c phase_4_handler comment block +
#    postmaster.c qvotec_spawn_enabled gate.
#
#    Per CLAUDE.md rule 8 (实现完整性 — 要么完整实现，要么显式拒绝),
#    this file is a placeholder that explicitly skips with reason.
#    Real L1-L8 implementation lands in the D8 follow-up hardening
#    round once QVOTEC successfully spawns under postmaster.
#
# IDENTIFICATION
#    src/test/cluster_tap/t/096_quorum_2node_round_trip.pl
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use Test::More;

plan skip_all =>
	'spec-2.6 96 — 2-node ClusterPair runtime round-trip TAP.  qvotec '
  . 'real poll body (P1.3) + D8 phase 4 spawn integration are landed; '
  . 'single-node runtime is verified by 095_qvotec_skeleton.pl L12-L16. '
  . 'L1-L8 2-node scenarios (start_pair / both register / quorum_view '
  . '2 alive / SIGSTOP partition / collision FATAL via Q6 newer-self / '
  . 'boot epoch recovery / all-disk-fail fail-closed) require a 2-node '
  . 'ClusterPair test harness that does not yet exist in this tree '
  . '(spec-2.5 cssd_heartbeat_round_trip 2-node pattern is the closest '
  . 'precedent).  Lands in a follow-up hardening round once the '
  . 'ClusterPair harness is generalized for voting-disk scenarios.';
