# spec-2.3 D10 -- 2-node envelope round-trip TAP.
#
# First behavioural test that two pgrac instances exchange the
# spec-2.3 36-byte ClusterICEnvelope (replacing the spec-2.2 24-byte
# ClusterMsgHeader + per-target seq_no).  Uses ClusterPair helper.
#
# Test matrix (5 L#):
#   L1 ClusterPair start_pair OK -- both postmasters live (smoke
#      that envelope wire pivot did not break startup; relies on
#      076 for full HELLO + connected coverage)
#   L2 peer.state = connected on both sides within 10s + heartbeat
#      counts grow (sanity that envelope traffic actually flows)
#   L3 bytes_send / bytes_recv match envelope frame size: after the
#      one-time 64-byte HELLO, every heartbeat is exactly 36 bytes;
#      verify (bytes_send - 64) == heartbeat_send_count * 36 on
#      both sides
#   L4 pg_cluster_ic_msg_types view contains HEARTBEAT row
#      (msg_type=1, name='heartbeat', handler_present=true,
#      broadcast_ok=false) on both nodes -- spec-2.3 D5 register
#      at postmaster phase 1 (cluster_lmon_shmem_init)
#   L5 producer mask invariant: HEARTBEAT.allowed_producer_mask !=
#      0 (LMON-only producer; bitwise OR of CLUSTER_IC_PRODUCER_LMON)
#
# Spec authority: pgrac:specs/spec-2.3-* §4.2 D10 (2-node tier1
# envelope round-trip; 36-byte frame size + msg_type registration).
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


# spec-2.3 D1 §3.7 frame sizes (frozen ABI).
my $HELLO_BYTES = 64;
my $ENVELOPE_BYTES = 36;


{
	my $pair = PostgreSQL::Test::ClusterPair->new_pair('pgrac081');
	$pair->start_pair;

	# ============================================================
	# L1 -- both postmasters live (envelope pivot did not break
	# startup).
	# ============================================================
	is($pair->node0->safe_psql('postgres', 'SELECT 1'), '1',
		'L1 node0 postmaster accepts SQL after tier1+envelope startup');
	is($pair->node1->safe_psql('postgres', 'SELECT 1'), '1',
		'L1 node1 postmaster accepts SQL after tier1+envelope startup');

	# ============================================================
	# L2 -- connected on both sides + heartbeats flow.  Default
	# heartbeat interval = 1s; sleep 3s after connected guarantees
	# at least 2 heartbeats sent each direction.
	# ============================================================
	ok($pair->wait_for_peer_state(0, 1, 'connected', 10),
		'L2 node0 sees node1 connected within 10s (envelope wire)');
	ok($pair->wait_for_peer_state(1, 0, 'connected', 10),
		'L2 node1 sees node0 connected within 10s (envelope wire)');

	sleep 3;

	# ============================================================
	# L3 -- bytes_send / bytes_recv match 36-byte envelope frame.
	#
	# Per spec-2.3 §3.7 + frozen 64-byte HELLO ABI: the only frames
	# crossing the wire after handshake are heartbeat envelopes.
	# Therefore:
	#   bytes_send == HELLO_BYTES + heartbeat_send_count * 36
	#
	# This is the strictest assertion that the wire IS 36-byte
	# envelopes (not 24-byte spec-2.2 ClusterMsgHeader).  A leftover
	# 24-byte path would either fail the modulo or skew the count.
	# ============================================================
	my $node0_to_1_send = $pair->node0->safe_psql('postgres',
		'SELECT bytes_send FROM pg_cluster_ic_peers WHERE node_id = 1');
	my $node0_to_1_hb = $pair->node0->safe_psql('postgres',
		'SELECT heartbeat_send_count FROM pg_cluster_ic_peers WHERE node_id = 1');

	cmp_ok($node0_to_1_hb, '>=', 1,
		'L3 node0 heartbeat_send_count >= 1 toward node1');
	is( $node0_to_1_send,
		$HELLO_BYTES + $node0_to_1_hb * $ENVELOPE_BYTES,
		"L3 node0 bytes_send to node1 = HELLO(64) + heartbeat_send_count * 36 (envelope frame)");

	my $node1_to_0_send = $pair->node1->safe_psql('postgres',
		'SELECT bytes_send FROM pg_cluster_ic_peers WHERE node_id = 0');
	my $node1_to_0_hb = $pair->node1->safe_psql('postgres',
		'SELECT heartbeat_send_count FROM pg_cluster_ic_peers WHERE node_id = 0');

	cmp_ok($node1_to_0_hb, '>=', 1,
		'L3 node1 heartbeat_send_count >= 1 toward node0');
	is( $node1_to_0_send,
		$HELLO_BYTES + $node1_to_0_hb * $ENVELOPE_BYTES,
		"L3 node1 bytes_send to node0 = HELLO(64) + heartbeat_send_count * 36 (envelope frame)");

	# Recv side mirrors send side (TCP pair symmetry).
	my $node0_from_1_recv = $pair->node0->safe_psql('postgres',
		'SELECT bytes_recv FROM pg_cluster_ic_peers WHERE node_id = 1');
	my $node0_from_1_hbr = $pair->node0->safe_psql('postgres',
		'SELECT heartbeat_recv_count FROM pg_cluster_ic_peers WHERE node_id = 1');
	is( $node0_from_1_recv,
		$HELLO_BYTES + $node0_from_1_hbr * $ENVELOPE_BYTES,
		"L3 node0 bytes_recv from node1 = HELLO(64) + heartbeat_recv_count * 36");

	my $node1_from_0_recv = $pair->node1->safe_psql('postgres',
		'SELECT bytes_recv FROM pg_cluster_ic_peers WHERE node_id = 0');
	my $node1_from_0_hbr = $pair->node1->safe_psql('postgres',
		'SELECT heartbeat_recv_count FROM pg_cluster_ic_peers WHERE node_id = 0');
	is( $node1_from_0_recv,
		$HELLO_BYTES + $node1_from_0_hbr * $ENVELOPE_BYTES,
		"L3 node1 bytes_recv from node0 = HELLO(64) + heartbeat_recv_count * 36");

	# ============================================================
	# L4 -- pg_cluster_ic_msg_types view contains HEARTBEAT row on
	# both nodes (spec-2.3 D8).  Registration happens at postmaster
	# phase 1 (cluster_lmon_shmem_init); identical on every node.
	# ============================================================
	for my $idx (0 .. 1) {
		my $node = $idx == 0 ? $pair->node0 : $pair->node1;
		my $row = $node->safe_psql('postgres',
			q{SELECT msg_type || '|' || name || '|' ||
			        handler_present || '|' || broadcast_ok
			    FROM pg_cluster_ic_msg_types
			   WHERE msg_type = 1});
		is( $row, '1|heartbeat|t|f',
			"L4 node$idx HEARTBEAT row {msg_type=1, name=heartbeat, handler_present=t, broadcast_ok=f}");
	}

	# ============================================================
	# L5 -- producer mask invariant: HEARTBEAT.allowed_producer_mask
	# is non-zero (LMON-only producer per spec-2.3 D5;
	# CLUSTER_IC_PRODUCER_LMON is bit (1<<B_LMON)).  Diagnostic
	# only -- view does not expose which producer the bit is.
	# ============================================================
	my $mask = $pair->node0->safe_psql('postgres',
		q{SELECT allowed_producer_mask::int8
		    FROM pg_cluster_ic_msg_types
		   WHERE msg_type = 1});
	cmp_ok($mask, '>', 0,
		'L5 HEARTBEAT.allowed_producer_mask > 0 (LMON producer bit set)');

	$pair->stop_pair;
}


done_testing();
