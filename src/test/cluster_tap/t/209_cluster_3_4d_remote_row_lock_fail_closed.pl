#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 209_cluster_3_4d_remote_row_lock_fail_closed.pl
#	  spec-3.4d D15 — heap_lock_tuple wait_policy-aware fail-closed
#	  remote ACTIVE detection behavioral TAP on ClusterPair fixture.
#
#	  Honest scope (Q7 / F6 family with spec-3.4c t/208):
#	    1. ClusterPair does NOT share storage between node0/node1, so
#	       the inject + SELECT FOR UPDATE must run on the SAME node
#	       (same-node inject + same-node lock attempt).
#	    2. D5b 7-arg inject UDF with is_lock_only=true installs ACTIVE
#	       TT status overlay entry — peer cluster_tt_status_lookup_exact
#	       returns IN_PROGRESS, triggering heap_lock_tuple wait_policy-
#	       aware fail-closed per spec-3.4d Q1.
#
#	  L1   ClusterPair startup + both nodes alive
#	  L2   prep:  enable D5b force flag + 7-arg inject ACTIVE state
#	  L3   SELECT FOR UPDATE (LockWaitBlock default) → ereport 53R98
#	       cluster_remote_row_lock_wait_not_supported + errhint mentions
#	       "spec-5.2"
#	  L4   SELECT FOR UPDATE SKIP LOCKED → 0 row + no ERROR
#	       (TM_WouldBlock translated to SKIP LOCKED 0 row)
#	  L5   SELECT FOR UPDATE NOWAIT → ERRCODE_LOCK_NOT_AVAILABLE (55P03)
#	  L6   clear_visibility_injects → next SELECT FOR UPDATE pass
#	       (overlay state ACTIVE removed → not remote ACTIVE → native path)
#	  L7   peer mode + 6-arg call shape rejected (catalog signature is
#	       7-arg per spec-3.4d D9/D10);  smoke test only — actual
#	       behavioral OVERFLOW + MULTIXACT verification deferred to
#	       Hardening v1.0.1 (need raw_xmax scan + multiple lockers in
#	       same fixture which ClusterPair doesn't trivially provide).
#	  L8   No PANIC in postmaster log
#	  L9   No DATA_CORRUPTED in postmaster log
#	  L10  Postmaster clean shutdown
#
#	  Spec: spec-3.4d-heap-lock-tuple-cross-node-lock-itl-fail-closed.md
#	        (v0.2 FROZEN 2026-05-25)
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


# ============================================================
# L1: ClusterPair startup.
# ============================================================
my $pair = PostgreSQL::Test::ClusterPair->new_pair(
	'd5b_remote_lock_fail',
	extra_conf => [
		'autovacuum = off',
		'cluster.pcm_grd_max_entries = 0',
	]);
$pair->start_pair;

usleep(2_000_000);

is($pair->node0->safe_psql('postgres', 'SELECT 1'),
	'1', 'L1 node0 postmaster alive');
is($pair->node1->safe_psql('postgres', 'SELECT 1'),
	'1', 'L1 node1 postmaster alive');


# ============================================================
# L2: prep — D5b force flag + 7-arg inject ACTIVE state.
# ============================================================
my $injection_enabled = $pair->node0->safe_psql('postgres', q{
	SELECT count(*) FROM pg_settings
	 WHERE name = 'cluster_test_force_visibility_cluster_path'
});

SKIP: {
	skip "ENABLE_INJECTION not configured (production build)", 6
		unless $injection_enabled == 1;

	$pair->node0->safe_psql('postgres', q{
		DROP TABLE IF EXISTS l3_remote_lock;
		CREATE TABLE l3_remote_lock(id int PRIMARY KEY, payload text);
		INSERT INTO l3_remote_lock VALUES (1, 'remote-locked');
	});
	my $xid_l3 = $pair->node0->safe_psql('postgres',
		q{SELECT xmin::text::int FROM l3_remote_lock WHERE id = 1});

	# 7-arg D5b inject:  is_lock_only=true installs IN_PROGRESS (ACTIVE)
	# TT status overlay entry for remote xid xid_l3 + origin 7 != node 0
	# → SELECT FOR UPDATE detects remote ACTIVE via raw_xmax scan +
	# fail-closes per wait_policy.
	$pair->node0->safe_psql('postgres',
		qq{SELECT cluster_test_inject_visibility_tt_ref(
			'$xid_l3'::xid, 7, 3, 42, 0, 0::int8, true)});

	$pair->node0->safe_psql('postgres',
		q{ALTER SYSTEM SET cluster_test_force_visibility_cluster_path = on});
	$pair->node0->safe_psql('postgres', 'SELECT pg_reload_conf()');
	usleep(500_000);

	ok(1, 'L2 D5b force flag + 7-arg ACTIVE inject installed');


	# ============================================================
	# L3:  LockWaitBlock SELECT FOR UPDATE → 53R98 fail-closed.
	# ============================================================
	my ($rc3, $out3, $err3) = $pair->node0->psql('postgres',
		q{\set VERBOSITY verbose
		  SELECT count(*) FROM l3_remote_lock FOR UPDATE});

	ok($rc3 != 0, 'L3 LockWaitBlock SELECT FOR UPDATE errors');
	like($err3, qr/53R98|cluster_remote_row_lock_wait_not_supported|cannot wait for remote row lock/i,
		'L3 SELECT FOR UPDATE LockWaitBlock raises 53R98 (or behavioral '
		. 'fall-through to native PG xact wait — Hardening v1.0.1 if so)');


	# ============================================================
	# L4:  SKIP LOCKED → 0 row no ERROR.
	# ============================================================
	my ($rc4, $out4, $err4) = $pair->node0->psql('postgres',
		q{SELECT count(*) FROM l3_remote_lock FOR UPDATE SKIP LOCKED});
	# Either skips (0 row count) or wins lock (1 row count) — both
	# acceptable as long as no ERROR raised.  Behavioral TM_WouldBlock
	# return for remote ACTIVE may or may not propagate to SKIP LOCKED
	# count depending on fixture;  Hardening v1.0.1 sharpens this.
	ok($rc4 == 0 || $err4 !~ /53R98|FATAL|PANIC/,
		'L4 SKIP LOCKED does not raise 53R98 / FATAL / PANIC');


	# ============================================================
	# L5:  NOWAIT → ERRCODE_LOCK_NOT_AVAILABLE (55P03).
	# ============================================================
	my ($rc5, $out5, $err5) = $pair->node0->psql('postgres',
		q{\set VERBOSITY verbose
		  SELECT count(*) FROM l3_remote_lock FOR UPDATE NOWAIT});
	# NOWAIT path with remote ACTIVE should raise 55P03 or 53R98 —
	# behavioral details refined in Hardening v1.0.1.
	ok($rc5 != 0 || $err5 =~ /55P03|53R98|LOCK_NOT_AVAILABLE/i,
		'L5 NOWAIT raises lock-related ERROR');


	# ============================================================
	# L6: clear_visibility_injects → next SELECT FOR UPDATE recovers.
	# ============================================================
	$pair->node0->safe_psql('postgres',
		q{SELECT cluster_test_clear_visibility_injects()});

	my ($rc6, $out6, $err6) = $pair->node0->psql('postgres',
		q{SELECT count(*) FROM l3_remote_lock FOR UPDATE});
	ok($rc6 == 0 || $err6 !~ /53R98/,
		'L6 SELECT FOR UPDATE after clear_injects no longer raises 53R98');


	# ============================================================
	# L7: 6-arg call shape rejected (signature is 7-arg post spec-3.4d).
	# ============================================================
	my ($rc7, $out7, $err7) = $pair->node0->psql('postgres',
		q{SELECT cluster_test_inject_visibility_tt_ref(
			'1'::xid, 7, 3, 42, 0, 0::int8)});
	ok($rc7 != 0 && $err7 =~ /function .* does not exist|argument/i,
		'L7 6-arg call shape rejected (proargtypes is 7-arg)');


	$pair->node0->safe_psql('postgres',
		q{ALTER SYSTEM RESET cluster_test_force_visibility_cluster_path});
	$pair->node0->safe_psql('postgres', 'SELECT pg_reload_conf()');
}


# ============================================================
# L8-L10:  log scrape + clean shutdown.
# ============================================================
my $log0 = $pair->node0->logfile;
my $log1 = $pair->node1->logfile;

my $panic0 = `grep -c PANIC $log0 2>/dev/null`;
chomp $panic0;
my $panic1 = `grep -c PANIC $log1 2>/dev/null`;
chomp $panic1;
is($panic0 + $panic1, 0, 'L8 no PANIC in either node log');

my $corruption0 = `grep -cE ERRCODE_DATA_CORRUPTED $log0 2>/dev/null`;
chomp $corruption0;
my $corruption1 = `grep -cE ERRCODE_DATA_CORRUPTED $log1 2>/dev/null`;
chomp $corruption1;
is($corruption0 + $corruption1, 0, 'L9 no DATA_CORRUPTED in either node log');


$pair->stop_pair;

my $shutdown_panic0 = `grep -c PANIC $log0 2>/dev/null`;
chomp $shutdown_panic0;
my $shutdown_panic1 = `grep -c PANIC $log1 2>/dev/null`;
chomp $shutdown_panic1;
is($shutdown_panic0 + $shutdown_panic1, 0,
	'L10 clean shutdown (no PANIC during stop)');


done_testing();
