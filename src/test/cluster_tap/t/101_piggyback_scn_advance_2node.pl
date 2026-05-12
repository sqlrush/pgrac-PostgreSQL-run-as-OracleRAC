#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 101_piggyback_scn_advance_2node.pl
#	  spec-2.10 D7: 2-node end-to-end TAP for SCN piggyback observability.
#
#	  Strategy (v0.2 P1.2 修订):  不依赖 idle heartbeat 推 observe bump
#	  (cluster_scn_observe CAS-only-on-advance,idle heartbeat 不 bump).
#	  改用 node0 主动 induce SCN advance + 降低 BOC 干扰 + assert node1
#	  观察到 SCN 增长 + observe bump.
#
#	  Weakest provable statement:  "BOC 低频化后,真实 SCN advance 能通过
#	  已存在 IC envelope piggyback 被远端 observe".
#
#	  L1-L7:
#	    L1 ClusterPair strict-mode setup + tier1 connected baseline +
#	       heartbeat baseline (per L99 substrate verify)
#	    L2 snapshot node1 receiver-side baseline counters
#	    L3 verify default cluster.boc_sweep_interval_ms = 100ms, then
#	       ALTER SYSTEM SET it to 1000ms on both nodes + pg_reload_conf
#	    L4 on node0 loop 5x:  CREATE TABLE / DROP TABLE (>= 10 SCN advance)
#	    L5 wait 8s real time for LMON drain + envelope propagation
#	    L6 snapshot after counters on node1;  assert deltas > 0
#	    L7 cleanup
#
#	  NOT verified (v0.2 P1.2 收紧):
#	    - bump_count_delta ≈ heartbeat_recv_count_delta 1:1 (observe is
#	      CAS-only-on-advance, idle heartbeat doesn't bump)
#	    - isolate heartbeat-only (BOC 仍 ~1/s 存在)
#	    - boc_broadcast_fanout_count_delta quantitative dial-down (IC
#	      fanout cadence 仍 ~1/s by LMON tick,不动)
#
#	  Spec authority: pgrac:specs/spec-2.10-scn-piggyback-observability.md
#	  (frozen v0.2 Q1-Q6 2026-05-12) — §4.2.
#
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
# Portions Copyright (c) 2026, pgrac contributors
#
# Author: SqlRush <sqlrush@gmail.com>
#
# IDENTIFICATION
#	  src/test/cluster_tap/t/101_piggyback_scn_advance_2node.pl
#
# NOTES
#	  This is a pgrac-original file.
#	  Spec: spec-2.10-scn-piggyback-observability.md (frozen v0.2)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::ClusterPair;
use PostgreSQL::Test::Utils;
use Test::More;


# Helper: read a single counter from pg_cluster_state via SRF dump.
sub pg_cluster_state_int
{
	my ($node, $category, $key) = @_;
	my $sql = qq{
		SELECT COALESCE(value, '0')::bigint
		FROM pg_cluster_state
		WHERE category = '$category' AND key = '$key'
	};
	return $node->safe_psql('postgres', $sql);
}


# ----------
# L1: setup — strict-mode ClusterPair with 3 voting disks.
# ----------
my $pair = PostgreSQL::Test::ClusterPair->new_pair(
	'piggyback_scn_advance', quorum_voting_disks => 3);
$pair->start_pair;

is($pair->node0->safe_psql('postgres', 'SELECT 1'),
	'1', 'L1 node0 postmaster alive');
is($pair->node1->safe_psql('postgres', 'SELECT 1'),
	'1', 'L1 node1 postmaster alive');

# Tier1 connected baseline (30s timeout per spec-2.9 retag2 Ubuntu CI fix).
ok($pair->wait_for_peer_state(0, 1, 'connected', 30),
	'L1 node0 sees node1 connected (tier1 substrate up)');
ok($pair->wait_for_peer_state(1, 0, 'connected', 30),
	'L1 node1 sees node0 connected (tier1 substrate up)');

# Heartbeat baseline (per L99 substrate verify).
sleep 3;
is($pair->heartbeat_seen(0, 1), 't',
	'L1 node0 heartbeat_send_count > 0 toward node1 (IC alive)');
is($pair->heartbeat_seen(1, 0), 't',
	'L1 node1 heartbeat_send_count > 0 toward node0 (IC alive)');


# ----------
# L2: snapshot node1 receiver-side baseline counters.
# ----------
my $max_remote_pre = pg_cluster_state_int($pair->node1,
	'scn', 'scn_max_observed_remote');
my $bump_count_pre = pg_cluster_state_int($pair->node1,
	'scn', 'scn_observe_bump_count');

# Optional per-peer attribution: from node0 peer row on node1.
my $lamport_pre = $pair->node1->safe_psql('postgres',
	"SELECT COALESCE(lamport_observe_advance_count, 0)::bigint "
		. "FROM pg_cluster_ic_peers WHERE node_id = 0");

note "L2 baseline: max_observed_remote=$max_remote_pre "
	. "observe_bump_count=$bump_count_pre "
	. "lamport_observe_advance_count(from node0)=$lamport_pre";


# ----------
# L3: verify default via SHOW, then ALTER SYSTEM SET + pg_reload_conf.
#	Spec-2.10 D6 P2 moved real GUC default verification from standalone
#	unit tests to TAP because test_cluster_scn only sees a stub variable.
# ----------
my $default_node0 = $pair->node0->safe_psql('postgres',
	"SHOW cluster.boc_sweep_interval_ms");
my $default_node1 = $pair->node1->safe_psql('postgres',
	"SHOW cluster.boc_sweep_interval_ms");
is($default_node0, '100ms',
	"L3 node0 SHOW cluster.boc_sweep_interval_ms default is 100ms");
is($default_node1, '100ms',
	"L3 node1 SHOW cluster.boc_sweep_interval_ms default is 100ms");

for my $node ($pair->node0, $pair->node1)
{
	$node->safe_psql('postgres',
		"ALTER SYSTEM SET cluster.boc_sweep_interval_ms = 1000");
	$node->reload;
}
sleep 1;	# allow SIGHUP processing

my $show_node0 = $pair->node0->safe_psql('postgres',
	"SHOW cluster.boc_sweep_interval_ms");
my $show_node1 = $pair->node1->safe_psql('postgres',
	"SHOW cluster.boc_sweep_interval_ms");
# PG GUC unit display normalizes 1000ms → "1s" (GUC_UNIT_MS auto-fold
# at boundary).  Accept either form; key is that reload happened and
# value is exactly 1s = 1000ms.
ok($show_node0 eq '1s' || $show_node0 eq '1000ms',
	"L3 node0 SHOW cluster.boc_sweep_interval_ms reload OK (got '$show_node0')");
ok($show_node1 eq '1s' || $show_node1 eq '1000ms',
	"L3 node1 SHOW cluster.boc_sweep_interval_ms reload OK (got '$show_node1')");


# ----------
# L4: on node0 loop 5x — induce SCN advance via CREATE/DROP commits.
# ----------
my $scn_pre = pg_cluster_state_int($pair->node0,
	'scn', 'scn_current_local');

for my $i (1 .. 5)
{
	$pair->node0->safe_psql('postgres',
		"CREATE TABLE t_scn101_iter_$i (x int)");
	$pair->node0->safe_psql('postgres',
		"DROP TABLE t_scn101_iter_$i");
}

my $scn_post = pg_cluster_state_int($pair->node0,
	'scn', 'scn_current_local');

ok($scn_post > $scn_pre,
	"L4 node0 SCN advanced after 5x CREATE/DROP "
		. "(pre=$scn_pre post=$scn_post)");


# ----------
# L5: wait 8s real time (LMON drain + envelope propagation + recv CAS).
# ----------
sleep 8;


# ----------
# L6: snapshot after counters on node1 + verify deltas > 0.
# ----------
my $max_remote_post = pg_cluster_state_int($pair->node1,
	'scn', 'scn_max_observed_remote');
my $bump_count_post = pg_cluster_state_int($pair->node1,
	'scn', 'scn_observe_bump_count');
my $lamport_post = $pair->node1->safe_psql('postgres',
	"SELECT COALESCE(lamport_observe_advance_count, 0)::bigint "
		. "FROM pg_cluster_ic_peers WHERE node_id = 0");

my $max_remote_delta = $max_remote_post - $max_remote_pre;
my $bump_count_delta = $bump_count_post - $bump_count_pre;
my $lamport_delta = $lamport_post - $lamport_pre;

note "L6 post: max_observed_remote=$max_remote_post "
	. "observe_bump_count=$bump_count_post "
	. "lamport_observe_advance_count=$lamport_post";
note "L6 deltas: max_remote=+$max_remote_delta "
	. "bump_count=+$bump_count_delta "
	. "lamport=+$lamport_delta";

# Mandatory (a): node0 SCN advance propagated to node1 via piggyback.
ok($max_remote_delta > 0,
	"L6a node1 scn_max_observed_remote advanced "
		. "(piggyback真传到 receiver;+$max_remote_delta)");

# Mandatory (b): CAS-Lamport bump truly fired on node1 recv path.
ok($bump_count_delta > 0,
	"L6b node1 scn_observe_bump_count incremented "
		. "(CAS-Lamport bump 真 fire;+$bump_count_delta)");

# Optional (c): per-peer attribution from node0 peer row.
ok($lamport_delta > 0,
	"L6c node1 per-peer lamport_observe_advance_count "
		. "(from node0) advanced (+$lamport_delta)");


# ----------
# L7: cleanup — reset GUC + stop_pair.
# ----------
for my $node ($pair->node0, $pair->node1)
{
	$node->safe_psql('postgres',
		"ALTER SYSTEM RESET cluster.boc_sweep_interval_ms");
	$node->reload;
}
$pair->stop_pair;

done_testing();
