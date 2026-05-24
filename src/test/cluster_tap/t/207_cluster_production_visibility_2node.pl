#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 207_cluster_production_visibility_2node.pl
#	  spec-3.4b D13 — production cross-node visibility behavioral TAP on
#	  2-node ClusterPair.  spec-3.4b ship's raison d'être evidence test.
#
#	  Methodology:
#	    * Use pg_cluster_state(category='tt_status') counters
#	      (install_count, self_consumer_hit_count, lookup_hit_count,
#	      flush_count) to verify the production binding/install/lookup
#	      chain actually fires, not just "did not crash".
#	    * Use pg_cluster_state(category='tt_status_hint') emit_count /
#	      receive_count to verify cross-node wire propagation.
#	    * Each L test takes a baseline counter snapshot, runs the DML,
#	      then asserts the delta.
#
#	  L1   ClusterPair startup + both nodes alive + Q4 HC activation
#	       flush observable (flush_count >= 1 after boot)
#	  L2   node0 INSERT increments tt_status.install_count by 1
#	       (binding created + status installed; F11 + F7 working)
#	  L3   node0 INSERT also bumps tt_status.self_consumer_hit_count
#	       (spec-3.1 v0.4 N7 self-consumer lookup fires)
#	  L4   node0 INSERT emits tt_status_hint.emit_count (cross-node
#	       wire propagation; spec-3.2 D4)
#	  L5   node0 INSERT + COMMIT → row visible to node1 via SELECT
#	       (production cluster path returns real triple → spec-3.2 D5
#	       enters cluster path → spec-3.3 D10 decide_by_scn → VISIBLE)
#	  L6   node0 INSERT + ROLLBACK → row invisible to node1
#	  L7   node1 receives the hint (receive_count > 0 after L4)
#	  L8   100-row multi-page INSERT increments install_count by 1
#	       (F11 xact-local binding: single install for whole xact)
#	  L9   Cross-page UPDATE shares the same binding (F11; no
#	       binding-mismatch WARNING in node0 logs)
#	  L10  100-row batch fully visible to node1 after commit
#	  L11  Aborted 100-row batch fully invisible to node1
#	  L12  Same-xact INSERT+UPDATE+UPDATE → install_count += 1 only
#	       (F11 binding reuse across multiple DML)
#	  L13  logical decoding regress sanity (pg_logical_emit_message
#	       does not crash after cluster DML)
#	  L14  parallel worker scan after cross-node commit returns full
#	       row count (binding propagates through parallel snapshot)
#	  L15  baselines sanity: pg_cluster_state.shmem.region_count == 37
#	       (D14 ripple verified)
#	  L16  legacy InvalidUba fallback: empty table SELECT cross-node
#	       does not raise + returns 0 rows (backward-compat smoke)
#
#	  Spec: spec-3.4b-real-tt-allocator-uba-encoding-production-cross-node.md
#	        (v0.3 FROZEN 2026-05-24)
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
use Time::HiRes qw(usleep);


# Helper: read one tt_status / tt_status_hint counter from a node.
sub counter_value
{
	my ($node, $category, $key) = @_;
	my $v = $node->safe_psql('postgres',
		qq{SELECT value FROM pg_cluster_state WHERE category='$category' AND key='$key'});
	return defined($v) && $v ne '' ? $v : '0';
}

sub counter_delta
{
	my ($node, $category, $key, $before) = @_;
	my $after = counter_value($node, $category, $key);
	return $after - $before;
}


# ============================================================
# L1: ClusterPair startup + Q4 HC activation flush observable.
# ============================================================
my $pair = PostgreSQL::Test::ClusterPair->new_pair(
	'prod_visibility',
	extra_conf => [
		'autovacuum = off',
		'cluster.pcm_grd_max_entries = 0',	# L175 fixture isolation
	]);
$pair->start_pair;

usleep(2_000_000);

is($pair->node0->safe_psql('postgres', 'SELECT 1'),
	'1', 'L1 node0 postmaster alive');
is($pair->node1->safe_psql('postgres', 'SELECT 1'),
	'1', 'L1 node1 postmaster alive');

# Q4 HC: cluster_tt_status_flush_all_at_activation was called during
# cluster_init_shmem -- flush_count should be >= 1 even before any DML.
my $l1_flush_n0 = counter_value($pair->node0, 'tt_status', 'flush_count');
ok($l1_flush_n0 >= 1,
	"L1 node0 tt_status.flush_count >= 1 (Q4 HC activation flush fired, got $l1_flush_n0)");

my $l1_flush_n1 = counter_value($pair->node1, 'tt_status', 'flush_count');
ok($l1_flush_n1 >= 1,
	"L1 node1 tt_status.flush_count >= 1 (Q4 HC activation flush fired, got $l1_flush_n1)");


# ============================================================
# L2: node0 INSERT increments install_count by 1.
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l2_install;
	CREATE TABLE l2_install(id int);
});

my $l2_install_before = counter_value($pair->node0, 'tt_status', 'install_count');
$pair->node0->safe_psql('postgres', q{INSERT INTO l2_install VALUES (1)});
my $l2_install_delta = counter_delta($pair->node0, 'tt_status', 'install_count',
									 $l2_install_before);
ok($l2_install_delta >= 1,
	"L2 node0 install_count delta >= 1 (binding + status install fired, got $l2_install_delta)");


# ============================================================
# L3: node0 INSERT bumps self_consumer_hit_count.
# ============================================================
my $l3_self_before = counter_value($pair->node0, 'tt_status', 'self_consumer_hit_count');
$pair->node0->safe_psql('postgres', q{INSERT INTO l2_install VALUES (2)});
my $l3_self_delta = counter_delta($pair->node0, 'tt_status', 'self_consumer_hit_count',
								  $l3_self_before);
ok($l3_self_delta >= 1,
	"L3 self_consumer_hit_count delta >= 1 (spec-3.1 N7 self-consumer lookup fired, got $l3_self_delta)");


# ============================================================
# L4: node0 INSERT emits cross-node hint (emit_count).
# ============================================================
my $l4_emit_before = counter_value($pair->node0, 'tt_status_hint', 'emit_count');
$pair->node0->safe_psql('postgres', q{INSERT INTO l2_install VALUES (3)});
my $l4_emit_delta = counter_delta($pair->node0, 'tt_status_hint', 'emit_count',
								  $l4_emit_before);
ok($l4_emit_delta >= 1,
	"L4 tt_status_hint.emit_count delta >= 1 (cross-node wire fired, got $l4_emit_delta)");


# ============================================================
# L5: node0 INSERT + COMMIT → visible to node1.
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l5_visible;
	CREATE TABLE l5_visible(id int);
	INSERT INTO l5_visible VALUES (42);
});
usleep(500_000);	# small settle for cross-node propagation

my $l5_count = $pair->node1->safe_psql('postgres', 'SELECT count(*) FROM l5_visible');
is($l5_count, '1', 'L5 node1 sees 1 row after node0 INSERT + COMMIT');


# ============================================================
# L6: node0 ROLLBACK invisibility.
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l6_rollback;
	CREATE TABLE l6_rollback(id int);
});
$pair->node0->safe_psql('postgres', q{
	BEGIN;
	INSERT INTO l6_rollback VALUES (99);
	ROLLBACK;
});
my $l6_count = $pair->node1->safe_psql('postgres', 'SELECT count(*) FROM l6_rollback');
is($l6_count, '0', 'L6 node1 sees 0 rows after node0 rollback');


# ============================================================
# L7: node1 receive_count > 0 after the L4 emit.
# ============================================================
my $l7_recv_n1 = counter_value($pair->node1, 'tt_status_hint', 'receive_count');
ok($l7_recv_n1 >= 1,
	"L7 node1 tt_status_hint.receive_count >= 1 (wire chain fully exercised, got $l7_recv_n1)");


# ============================================================
# L8: 100-row multi-page INSERT: F11 binding => install_count += 1
#     for the WHOLE xact (not per-row).
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l8_batch;
	CREATE TABLE l8_batch(id int, payload text);
});

my $l8_install_before = counter_value($pair->node0, 'tt_status', 'install_count');
$pair->node0->safe_psql('postgres', q{
	INSERT INTO l8_batch SELECT g, repeat('x', 200) FROM generate_series(1, 100) g;
});
my $l8_install_delta = counter_delta($pair->node0, 'tt_status', 'install_count',
									 $l8_install_before);
ok($l8_install_delta == 1,
	"L8 install_count delta == 1 for 100-row INSERT (F11 binding shared across xact, got $l8_install_delta)");


# ============================================================
# L9: cross-page UPDATE shares binding (no stale-binding WARNING).
# ============================================================
my ($l9_rc, $l9_out, $l9_err) = $pair->node0->psql('postgres', q{
	UPDATE l8_batch SET payload = repeat('y', 200) WHERE id <= 50;
});
is($l9_rc, 0, 'L9 cross-page UPDATE returns 0');
unlike($l9_err, qr/cluster_tt_local: stale binding/,
	'L9 no stale-binding WARNING (F11 binding reuse held across pages)');


# ============================================================
# L10: 100-row batch visible to node1.
# ============================================================
my $l10_count = $pair->node1->safe_psql('postgres', 'SELECT count(*) FROM l8_batch');
is($l10_count, '100', 'L10 node1 sees all 100 rows of multi-page batch');


# ============================================================
# L11: aborted 100-row batch invisible to node1.
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l11_abort_batch;
	CREATE TABLE l11_abort_batch(id int);
});
$pair->node0->safe_psql('postgres', q{
	BEGIN;
	INSERT INTO l11_abort_batch SELECT g FROM generate_series(1, 100) g;
	ROLLBACK;
});
my $l11_count = $pair->node1->safe_psql('postgres',
	'SELECT count(*) FROM l11_abort_batch');
is($l11_count, '0', 'L11 node1 sees 0 rows after 100-row rollback');


# ============================================================
# L12: same-xact INSERT+UPDATE+UPDATE → install_count += 1.
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l12_binding_reuse;
	CREATE TABLE l12_binding_reuse(id int, v text);
});

my $l12_install_before = counter_value($pair->node0, 'tt_status', 'install_count');
my ($l12_rc, $l12_out, $l12_err) = $pair->node0->psql('postgres', q{
	BEGIN;
	INSERT INTO l12_binding_reuse VALUES (1, 'a');
	UPDATE l12_binding_reuse SET v = 'b' WHERE id = 1;
	UPDATE l12_binding_reuse SET v = 'c' WHERE id = 1;
	COMMIT;
});
is($l12_rc, 0, 'L12 same-xact INSERT+UPDATE+UPDATE returns 0');
my $l12_install_delta = counter_delta($pair->node0, 'tt_status', 'install_count',
									  $l12_install_before);
ok($l12_install_delta == 1,
	"L12 install_count delta == 1 for INSERT+UPDATE+UPDATE in one xact (F11 binding reused, got $l12_install_delta)");


# ============================================================
# L13: logical decoding sanity.
# ============================================================
my ($l13_rc, $l13_out, $l13_err) = $pair->node0->psql('postgres', q{
	SELECT pg_logical_emit_message(true, 'spec-3.4b', 'cross-node test');
});
is($l13_rc, 0, 'L13 pg_logical_emit_message after cluster DML did not crash');


# ============================================================
# L14: parallel worker scan.
# ============================================================
my $l14_count = $pair->node1->safe_psql('postgres', q{
	SET max_parallel_workers_per_gather = 2;
	SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0;
	SELECT count(*) FROM l8_batch;
});
is($l14_count, '100', 'L14 parallel worker scan returns full row count on cross-node data');


# ============================================================
# L15: baselines (region_count after D14 ripple).
# ============================================================
my $l15_region_count = $pair->node0->safe_psql('postgres', q{
	SELECT value FROM pg_cluster_state
	WHERE category='shmem' AND key='region_count'
});
is($l15_region_count, '37',
	'L15 pg_cluster_state.shmem.region_count == 37 (D14 ripple verified)');


# ============================================================
# L16: legacy InvalidUba fallback (backward-compat smoke).
# ============================================================
$pair->node0->safe_psql('postgres', q{
	DROP TABLE IF EXISTS l16_empty;
	CREATE TABLE l16_empty(id int);
});
my ($l16_rc, $l16_out, $l16_err) = $pair->node1->psql('postgres', q{
	SELECT count(*) FROM l16_empty;
});
is($l16_rc, 0, 'L16 node1 SELECT on empty table did not crash');
is($l16_out, '0', 'L16 node1 sees 0 rows (PG-native fallback path)');
unlike($l16_err, qr/ERRCODE_DATA_CORRUPTED|53R97/,
	'L16 no DATA_CORRUPTED / 53R97 in legacy InvalidUba fallback');


$pair->stop_pair;

done_testing();
