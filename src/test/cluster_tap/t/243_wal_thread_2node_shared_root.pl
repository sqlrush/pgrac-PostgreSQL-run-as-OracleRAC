#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 243_wal_thread_2node_shared_root.pl
#    spec-4.1 -- per-thread WAL layout, 2-node shared-root surface.
#
#    Substrate (L92 honesty): both postmasters run on one host and share
#    one real directory tree (the voting-disk harness pattern) -- a true
#    shared filesystem for the WAL root, not cross-host shared storage.
#
#      L1  ClusterPair(wal_threads_root => 1): thread_1 + thread_2 under
#          one shared root, both nodes validate + claim their stream
#      L2  concurrent writes on both nodes; per-node stamp counters grow
#          independently (no shared write coordination, AD-009)
#      L3  cross-thread read: node0's pg_waldump reads node1's stream at
#          <root>/thread_2 over an explicit window (all-readable)
#      L4  scope note: ANY data access around a pair-node restart is
#          blocked by a pre-existing Stage-2 GCS gap (= roadmap 4.7,
#          two signatures documented inline); restart legs therefore
#          assert via the node-local SRF only.  Crash-cycle coverage
#          of a routed stream lives in t/242 L4 (single node).
#      L5  foreign claim: node0's claim copied over node1's -> node1
#          start refused (53RA1 "claimed by node 0"); restore -> ok;
#          claim is write-once across restarts (claim_created=f)
#      L6  thread directory missing under a different root -> start
#          refused (53RA0; directories are bootstrap-created, never
#          auto-mkdir'd); restore -> ok
#      L7  stamps are distinct per stream: thread: 1 vs thread: 2
#      L8  both claim files are the 40-byte v1 layout (storage-level
#          ownership artifacts present and well-formed)
#
#    Author: SqlRush <sqlrush@gmail.com>
#    Spec: spec-4.1-per-thread-wal-routing.md (FROZEN v1.0)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use File::Copy qw(copy);
use PgracClusterNode;
use PostgreSQL::Test::ClusterPair;
use PostgreSQL::Test::Utils;
use Test::More;

sub dumpkey
{
	my ($node, $key) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT value FROM pg_cluster_state
		WHERE category='wal_thread' AND key='$key'});
}

sub window_dump
{
	my ($node, $waldir, $lsn0, $lsn1) = @_;
	# L227: explicit --start/--end window at quiesced points.
	my ($out, $err) = run_command([
		$node->installed_command('pg_waldump'),
		'--path' => $waldir,
		'--start' => $lsn0,
		'--end' => $lsn1 ]);
	return $out;
}

my $pair = PostgreSQL::Test::ClusterPair->new_pair('walthreads',
	wal_threads_root => 1);
my $root = $pair->wal_threads_root;
my $node0 = $pair->node0;
my $node1 = $pair->node1;

$pair->start_pair;

# ============================================================
# L1: both nodes validated + claimed their thread directories.
# ============================================================
is(dumpkey($node0, 'thread_id'), '1', 'L1 node0 is thread 1');
is(dumpkey($node1, 'thread_id'), '2', 'L1 node1 is thread 2');
is(dumpkey($node0, 'dir_validated'), 't', 'L1 node0 routing validated');
is(dumpkey($node1, 'dir_validated'), 't', 'L1 node1 routing validated');
ok(-f "$root/thread_1/pgrac_thread.claim", 'L1 thread_1 claimed');
ok(-f "$root/thread_2/pgrac_thread.claim", 'L1 thread_2 claimed');

# ============================================================
# L2: concurrent writes; independent stamp accumulators.
#
# Shared-nothing harness hazard: the FIRST user relation created on
# each node gets the same relfilenode, so cluster-wide GCS block keys
# collide across what are really two distinct tables and the invalidate
# protocol churns between the two DML streams (HC116 timeouts under
# load).  Offset node1's relfilenode allocation with pad relations so
# the two t243 tables key disjoint GCS resources.
# ============================================================
$node1->safe_psql('postgres',
	q{CREATE TABLE pad_a (x int); CREATE TABLE pad_b (x int); CREATE TABLE pad_c (x int)});

my $s0 = dumpkey($node0, 'page_stamp_count');
my $s1 = dumpkey($node1, 'page_stamp_count');
$node0->safe_psql('postgres',
	q{CREATE TABLE t243 AS SELECT g, 'a'||g AS v FROM generate_series(1, 400) g});
$node1->safe_psql('postgres',
	q{CREATE TABLE t243 AS SELECT g, 'b'||g AS v FROM generate_series(1, 400) g});
cmp_ok(dumpkey($node0, 'page_stamp_count'), '>', $s0, 'L2 node0 stamps grow');
cmp_ok(dumpkey($node1, 'page_stamp_count'), '>', $s1, 'L2 node1 stamps grow');

# ============================================================
# L3 + L7: cross-thread windowed reads; distinct stamps per stream.
# ============================================================
my $l0a = $node0->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$node0->safe_psql('postgres', q{INSERT INTO t243 SELECT g, 'c'||g FROM generate_series(401, 500) g});
my $l0b = $node0->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');

my $l1a = $node1->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$node1->safe_psql('postgres', q{INSERT INTO t243 SELECT g, 'd'||g FROM generate_series(401, 500) g});
my $l1b = $node1->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');

# node0 reading node1's stream by path (AD-009 all-readable layout);
# the reader-side default is accept-any-valid (spec-4.1 RL1).
my $cross = window_dump($node0, "$root/thread_2", $l1a, $l1b);
like($cross, qr/thread: 2/, 'L3 node0 cross-reads node1 stream (thread 2)');

my $own0 = window_dump($node0, "$root/thread_1", $l0a, $l0b);
like($own0, qr/thread: 1/, 'L7 thread_1 stream stamped 1');
unlike($own0, qr/thread: 2/, 'L7 thread_1 stream carries no thread-2 pages');
unlike($cross, qr/thread: 1/, 'L7 thread_2 stream carries no thread-1 pages');

# ============================================================
# L4 scope note -- node restart/crash data access on a pair is 4.7.
#
# A PRE-EXISTING Stage-2 gap (surfaced for the first time by this
# spec's TAP work) blocks any data access around a peer node restart:
#   (a) kill -9 + restart: heap redo in the startup process trips the
#       cluster_gcs_block hook ("MyBackendId=-1 out of [1,
#       MaxBackends]" FATAL) -- recovery cannot complete;
#   (b) even clean restart: the restarted node's first access to a
#       GCS-tracked block gets "master rejected transition_id=1 as
#       illegal" -- its in-memory GCS state is gone while the peer
#       retains the old view, and no rebuild protocol exists yet.
# Both are literally roadmap 4.7 (crash recovery of GCS/PCM: block PCM
# state + dirty buffer rebuild); registered as spec-4.1 ship notes /
# 4.7 D0 evidence.  Crash-cycle coverage of the ROUTED stream itself
# lives in t/242 L4 (single node, no peers).  The restart legs below
# (L5/L6) therefore assert through the node-local pg_cluster_state SRF
# only -- no table access after a restart.
# ============================================================
note('4.7 pre-existing gap: pair-node restart blocks data access '
	. '(crash: cluster_gcs_block MyBackendId=-1 FATAL in startup; '
	. 'clean: master rejected transition_id as illegal) -- '
	. 'see spec-4.1 ship notes; restart legs assert via SRF only');

# ============================================================
# L5: foreign claim (copied from thread_1) -> node1 refused.
# ============================================================
$node1->stop;
my $claim1 = "$root/thread_1/pgrac_thread.claim";
my $claim2 = "$root/thread_2/pgrac_thread.claim";
my $saved = do {
	open my $fh, '<:raw', $claim2 or die "open: $!";
	local $/; my $d = <$fh>; close $fh; $d;
};
copy($claim1, $claim2) or die "copy: $!";

my $log_off = -s $node1->logfile;
is($node1->start(fail_ok => 1), 0, 'L5 node1 refused with a foreign claim');
my $log = PostgreSQL::Test::Utils::slurp_file($node1->logfile, $log_off);
like($log, qr/is claimed by node 0 \(thread 1\)/,
	'L5 FATAL names the owning node (53RA1)');

open my $fh, '>:raw', $claim2 or die "open: $!";
print $fh $saved;
close $fh;
$node1->start;
is(dumpkey($node1, 'dir_validated'), 't', 'L5 restored claim validates again');
is(dumpkey($node1, 'claim_created'), 'f',
	'L5 claim is write-once: re-read on restart, never re-created');

# ============================================================
# L6: thread directory missing (different root) -> refused.
# ============================================================
$node1->stop;
my $otherroot = PostgreSQL::Test::Utils::tempdir();
$node1->append_conf('postgresql.conf',
	"cluster.wal_threads_dir = '$otherroot'\n");
$log_off = -s $node1->logfile;
is($node1->start(fail_ok => 1), 0, 'L6 node1 refused on missing thread dir');
$log = PostgreSQL::Test::Utils::slurp_file($node1->logfile, $log_off);
like($log, qr/WAL thread directory .* does not exist/,
	'L6 FATAL: directories are bootstrap-created, never auto-mkdir (53RA0)');
$node1->append_conf('postgresql.conf',
	"cluster.wal_threads_dir = '$root'\n");
$node1->start;
is(dumpkey($node1, 'dir_validated'), 't', 'L6 restored root validates again');

# ============================================================
# L8: claim artifacts well-formed (40-byte v1 layout).
# ============================================================
is(-s $claim1, 40, 'L8 thread_1 claim is 40 bytes');
is(-s $claim2, 40, 'L8 thread_2 claim is 40 bytes');

$pair->stop_pair;

done_testing();
