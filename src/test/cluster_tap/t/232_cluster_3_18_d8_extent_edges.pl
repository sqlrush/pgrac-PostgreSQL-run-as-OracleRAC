#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 232_cluster_3_18_d8_extent_edges.pl
#	  spec-3.18 D8 -- per-txn undo extent (D3) 8.A edge coverage.
#
#	  The happy path (sequential claim, degenerate undo_extent_blocks=1,
#	  retention recycle) is covered by test_cluster_undo_extent + t/214 + t/222.
#	  This test fills the two remaining 8.A extent edges:
#
#	  L1  (U-E2 cross-backend uniqueness):  two backends writing undo
#	      concurrently claim DISTINCT, non-overlapping extents off the shared
#	      next_extent_block (advanced under lifecycle_lock).  Overlapping extents
#	      would clobber one backend's undo / UBA chain, losing or corrupting its
#	      rows.  Both backends' disjoint INSERTs landing intact + extent_claim
#	      rising by >= 2 shows the extents stayed isolated.  Deterministic:
#	      single-statement INSERTs only (no multi-statement string), so it is not
#	      perturbed by host IPC state.
#	  L2  (U-E3 crash-mid-claim resume):  a backend claims a fresh extent and
#	      writes only the head of it (committed), then a crash leaves the rest
#	      claimed-but-unwritten.  A1 batch-marks the whole claimed range before
#	      any record write, so restart rebuilds the cursor from the on-disk
#	      used-bitmap (B1) after the claimed range rather than immediately
#	      reusing those unwritten blocks.  That is a bounded within-segment
#	      space cost, not a correctness leak: no UBA points at the unwritten
#	      blocks, and they are reclaimed when the whole segment recycles.  Restart
#	      must replay with no PANIC, keep the committed data, and let subsequent
#	      undo writes/reads resume correctly.
#
#	  Author: SqlRush <sqlrush@gmail.com>
#	  Spec: spec-3.18-write-path-performance-overhaul.md (D8 / U-E2 / U-E3)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('s318_d8ext');
$node->init;
$node->append_conf('postgresql.conf', "cluster.enabled = on\n");
$node->append_conf('postgresql.conf', "cluster.node_id = 0\n");
$node->append_conf('postgresql.conf', "cluster.allow_single_node = on\n");
$node->append_conf('postgresql.conf', "cluster.undo_buffers = 64\n");
$node->append_conf('postgresql.conf', "cluster.undo_extent_blocks = 4\n");
$node->append_conf('postgresql.conf', "checkpoint_timeout = 1h\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

my $counter = sub {
	my ($key) = @_;
	return $node->safe_psql('postgres',
		"SELECT value FROM cluster_dump_state() WHERE key = '$key'");
};

# ============================================================
# L1: cross-backend extent uniqueness (U-E2) -- deterministic, INSERT-only.
# ============================================================
{
	$node->safe_psql('postgres', 'CREATE TABLE l1(id int primary key, who int)');
	my $ec0 = $counter->('extent_claim_count');

	# Two backends, each in its own open transaction, INSERT disjoint key ranges
	# one statement at a time.  Interleaving the batches makes their extent
	# claims alternate off the shared cursor.
	my $s1 = $node->background_psql('postgres');
	my $s2 = $node->background_psql('postgres');
	$s1->query_safe('BEGIN');
	$s2->query_safe('BEGIN');
	for my $i (1 .. 5) {
		my ($lo1, $hi1) = (($i - 1) * 100 + 1, $i * 100);
		my ($lo2, $hi2) = (1000 + ($i - 1) * 100 + 1, 1000 + $i * 100);
		$s1->query_safe("INSERT INTO l1 SELECT g, 1 FROM generate_series($lo1, $hi1) g");
		$s2->query_safe("INSERT INTO l1 SELECT g, 2 FROM generate_series($lo2, $hi2) g");
	}
	$s1->query_safe('COMMIT');
	$s2->query_safe('COMMIT');
	$s1->quit;
	$s2->quit;

	# Overlapping extents would corrupt one backend's undo / UBA chain, surfacing
	# as missing or mis-tagged rows.  All disjoint rows intact == extents isolated.
	is($node->safe_psql('postgres', 'SELECT count(*) FROM l1'),
		'1000', 'L1 all rows from both concurrent backends present');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM l1 WHERE who = 1'),
		'500', 'L1 backend-1 rows intact (extent not clobbered by backend-2)');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM l1 WHERE who = 2'),
		'500', 'L1 backend-2 rows intact (extent not clobbered by backend-1)');
	cmp_ok($counter->('extent_claim_count'), '>=', $ec0 + 2,
		'L1 both backends claimed extents off the shared cursor');
}

# ============================================================
# L2: crash-mid-claim restart resume (U-E3).
# ============================================================
{
	$node->safe_psql('postgres', 'CREATE TABLE l2(id int primary key, v text)');
	$node->safe_psql('postgres', q{INSERT INTO l2 VALUES (1,'a'),(2,'b')});
	# Checkpoint so the post-checkpoint undo writes below must replay on restart.
	$node->safe_psql('postgres', 'CHECKPOINT');
	# A small committed write claims a fresh extent and writes only the head of
	# it; the rest of the extent stays claimed-but-unwritten but remains marked
	# used until whole-segment recycle (A1).
	$node->safe_psql('postgres', q{UPDATE l2 SET v = 'c' WHERE id = 1});

	$node->stop('immediate');    # crash: shmem next_extent_block cache is lost
	$node->start;                # restart: cursor rebuilt from the used-bitmap (B1)

	is($node->safe_psql('postgres', q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM l2}),
		'1:c,2:b', 'L2 committed data intact after crash-mid-claim restart');

	# Subsequent undo writes must resume cleanly: the bitmap-rebuilt cursor skips
	# the claimed-but-unwritten tail and new records land correctly.
	$node->safe_psql('postgres', q{INSERT INTO l2 VALUES (3,'d'),(4,'e')});
	$node->safe_psql('postgres', q{UPDATE l2 SET v = 'f' WHERE id = 2});
	is($node->safe_psql('postgres', q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM l2}),
		'1:c,2:f,3:d,4:e', 'L2 undo writes resume correctly after restart (no corruption)');
	unlike(slurp_file($node->logfile), qr/PANIC/,
		'L2 no redo PANIC on crash-mid-claim restart');
}

$node->stop;
done_testing();
