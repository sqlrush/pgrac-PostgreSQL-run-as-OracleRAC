#-------------------------------------------------------------------------
#
# 226_stage3_mvcc_acceptance_capability.pl
#    Stage 3.17 D1 — Stage 3 MVCC capability cross-cutting acceptance.
#
#    Ten capability classes (L1-L10), each a thin cross-cutting assertion
#    over an already-shipped Stage 3 surface (deep coverage lives in the
#    per-spec e2e t/213-225, which stay as regression assets):
#
#      L1   undo write-path:        own-instance DML emits undo records
#                                   (undo.record_alloc_count advances)
#      L2   CR block construction:  a held REPEATABLE READ snapshot reads
#                                   the pre-update row after a concurrent
#                                   commit (cr.cr_construct_count advances)
#      L3   durable TT slot:        a committed xact's row stays visible
#                                   (0x30 TT commit + tt_status surface)
#      L4   retention horizon:      retention/undo_cleaner counters readable
#                                   (ACTIVE retains; COMMITTED recyclable —
#                                   predicate proven in test_cluster_retention)
#      L5   undo cleaner:           UPDATE churn + cleaner counters readable
#      L6   visibility variants:    Update/Dirty/Self/Toast behaviours hold;
#                                   cross-node fork counters + 53R9H are
#                                   best-effort (need 2 nodes -> see t/223)
#      L7   2PC prepared:           PREPARE / COMMIT PREPARED / ROLLBACK
#                                   PREPARED visibility (twopc counters advance)
#      L8   recovery:               crash-restart redo idempotent, data intact
#      L9   SCN monotone:           own-instance scn_current_local advances
#      L10  metric matrix:          all 6 MVCC dump categories emit rows
#
#    Primary fixture is a single cluster-enabled node (pg_ctl level, like
#    t/225) so the bulk runs on this host;  the cross-node L6 legs degrade
#    to best-effort SKIP when only one node is present.  Smoke scope <= 2min.
#    Results accumulate into a Stage3AcceptanceReport JSON (D7).
#
# IDENTIFICATION
#    src/test/cluster_tap/t/226_stage3_mvcc_acceptance_capability.pl
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

use PgracClusterNode;
use PostgreSQL::Test::Stage3AcceptanceReport;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);


my $report = PostgreSQL::Test::Stage3AcceptanceReport->new(
	spec => '3.17', tag => $ENV{PGRAC_TAG} // 'unknown');

my $node = PgracClusterNode->new('s3acc');
$node->init;
$node->append_conf('postgresql.conf',
	"cluster.enabled = on\n"
	  . "cluster.node_id = 0\n"
	  . "cluster.allow_single_node = on\n"
	  . "autovacuum = off\n"
	  . "max_prepared_transactions = 10\n");
$node->start;


# Read a single cluster_debug counter (0 when absent).
sub _counter
{
	my ($cat, $key) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT COALESCE((SELECT value::bigint FROM pg_cluster_state
			WHERE category='$cat' AND key='$key'), 0)});
}

# Cross-cutting capability assertion:  run $sql, compare to $expect, record
# the verdict into the report (status PASS/FAIL, layer hard).
sub _assert_mvcc_capability
{
	my ($name, $sql, $expect) = @_;
	my $got = $node->safe_psql('postgres', $sql);
	my $pass = (defined $got && $got eq $expect);
	is($got, $expect, $name);
	$report->record_mvcc_capability($name,
		status => $pass ? 'PASS' : 'FAIL', layer => 'hard');
	return $pass;
}

# Best-effort capability note:  records SKIP + reason without a hard gate.
sub _besteffort_skip
{
	my ($name, $reason) = @_;
	$report->record_mvcc_capability($name,
		status => 'SKIP', layer => 'best_effort', reason => $reason);
	note("$name: best-effort SKIP — $reason");
}


# ============================================================
# L1: undo write-path — own-instance DML emits undo records.
# ============================================================
{
	$node->safe_psql('postgres', 'CREATE TABLE l1_undo (id int primary key, v text)');
	my $before = _counter('undo', 'record_alloc_count');
	$node->safe_psql('postgres',
		q{INSERT INTO l1_undo SELECT g, 'v' || g FROM generate_series(1, 100) g});
	$node->safe_psql('postgres', q{UPDATE l1_undo SET v = v || '_u' WHERE id <= 50});
	my $after = _counter('undo', 'record_alloc_count');
	ok($after > $before,
		"L1 undo write-path active (record_alloc_count $before -> $after)");
	$report->record_mvcc_capability('L1_undo_write_path',
		status => ($after > $before) ? 'PASS' : 'FAIL', layer => 'hard',
		counter_delta => ($after - $before));
}


# ============================================================
# L2: CR block construction — held snapshot reads pre-update row.
# ============================================================
{
	$node->safe_psql('postgres',
		'CREATE TABLE l2_cr (id int primary key, v int)');
	$node->safe_psql('postgres', 'INSERT INTO l2_cr VALUES (1, 100)');

	my $cr_before = _counter('cr', 'cr_construct_count');

	# Session A holds a REPEATABLE READ snapshot taken before the update.
	my $a = $node->background_psql('postgres', on_error_stop => 0);
	$a->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$a->query_safe('SELECT 1');	# establish the snapshot

	# Concurrent committed UPDATE from the main session.
	$node->safe_psql('postgres', 'UPDATE l2_cr SET v = 200 WHERE id = 1');

	# A still sees the pre-update value -> CR reconstructs the old version.
	my $seen = $a->query_safe('SELECT v FROM l2_cr WHERE id = 1');
	$a->query_safe('COMMIT');
	$a->quit;

	is($seen, '100', 'L2 CR: held snapshot reads pre-update row (100)');
	my $cr_after = _counter('cr', 'cr_construct_count');
	ok($cr_after > $cr_before,
		"L2 CR construct counter advanced ($cr_before -> $cr_after)");
	$report->record_mvcc_capability('L2_cr_block_construction',
		status => ($seen eq '100' && $cr_after > $cr_before) ? 'PASS' : 'FAIL',
		layer => 'hard', counter_delta => ($cr_after - $cr_before));
}


# ============================================================
# L3: durable TT slot — committed xact's row stays visible.
# ============================================================
{
	$node->safe_psql('postgres', q{
		CREATE TABLE l3_tt (id int primary key, v text);
		INSERT INTO l3_tt VALUES (1, 'committed');
	});
	_assert_mvcc_capability('L3_durable_tt_slot',
		q{SELECT v FROM l3_tt WHERE id = 1}, 'committed');
	# tt_status observability surface present (durable slot machinery).
	my $tt_keys = $node->safe_psql('postgres', q{
		SELECT count(*) FROM pg_cluster_state WHERE category='tt_status'});
	ok($tt_keys >= 1, "L3 tt_status observability surface present ($tt_keys keys)");
}


# ============================================================
# L4: retention horizon — predicate counters readable.
# ============================================================
{
	# ACTIVE retains / COMMITTED recyclable predicate is proven in
	# test_cluster_retention + D6 L7;  here we just confirm the retention /
	# undo_cleaner observability surface is live end-to-end.
	my $undo_cleaner_keys = $node->safe_psql('postgres', q{
		SELECT count(*) FROM pg_cluster_state WHERE category='undo_cleaner'});
	ok($undo_cleaner_keys >= 1,
		"L4 undo_cleaner/retention surface present ($undo_cleaner_keys keys)");
	$report->record_mvcc_capability('L4_retention_horizon',
		status => ($undo_cleaner_keys >= 1) ? 'PASS' : 'FAIL', layer => 'hard');
}


# ============================================================
# L5: undo cleaner — UPDATE churn + cleaner counters readable.
# ============================================================
{
	$node->safe_psql('postgres', q{
		CREATE TABLE l5_churn (id int primary key, v int);
		INSERT INTO l5_churn SELECT g, 0 FROM generate_series(1, 100) g;
	});
	# Churn the same rows to drive undo accumulation + cleaner work.
	for my $i (1 .. 5)
	{
		$node->safe_psql('postgres', "UPDATE l5_churn SET v = $i");
	}
	my $scan = _counter('undo_cleaner', 'cleaner_scan_count');
	my $cleaner_present = $node->safe_psql('postgres', q{
		SELECT count(*) FROM pg_cluster_state WHERE category='undo_cleaner'});
	ok($cleaner_present >= 1,
		"L5 undo cleaner surface present ($cleaner_present keys, scan=$scan)");
	$report->record_mvcc_capability('L5_undo_cleaner',
		status => ($cleaner_present >= 1) ? 'PASS' : 'FAIL', layer => 'hard');
}


# ============================================================
# L6: visibility variants — behaviours hold (single-node);
#     cross-node fork counters + 53R9H are best-effort (need 2 nodes).
# ============================================================
{
	# Update variant: UPDATE then read sees the new row.
	$node->safe_psql('postgres', q{
		CREATE TABLE l6_vis (id int primary key, v text);
		INSERT INTO l6_vis VALUES (1, 'a');
		UPDATE l6_vis SET v = 'b' WHERE id = 1;
	});
	_assert_mvcc_capability('L6_visibility_update',
		q{SELECT v FROM l6_vis WHERE id = 1}, 'b');

	# Self variant: a row inserted in the same xact is visible to it.  The
	# BEGIN block has no COMMIT, so psql auto-rolls-back at exit -> id=2 is
	# probed within its own xact and never persists.
	_assert_mvcc_capability('L6_visibility_self',
		q{BEGIN; INSERT INTO l6_vis VALUES (2, 'self');
		  SELECT v FROM l6_vis WHERE id = 2; }, 'self');

	# Toast variant: a large out-of-line value round-trips.
	$node->safe_psql('postgres', q{
		CREATE TABLE l6_toast (id int primary key, big text);
		INSERT INTO l6_toast VALUES (1, repeat('x', 100000));
	});
	_assert_mvcc_capability('L6_visibility_toast',
		q{SELECT length(big) FROM l6_toast WHERE id = 1}, '100000');

	# Cross-node fork counters + 53R9H cross-node write conflict need a
	# second node;  deep coverage is t/223 (ClusterPair).  Single-node own-
	# instance reads take the native PG path (no fork counter bump), so this
	# leg is honestly best-effort here.
	_besteffort_skip('L6_crossnode_fork_and_53R9H',
		'cross-node visibility fork + 53R9H need 2 nodes (see t/223 ClusterPair)');
}


# ============================================================
# L7: 2PC prepared — PREPARE / COMMIT PREPARED / ROLLBACK PREPARED.
# ============================================================
{
	$node->safe_psql('postgres',
		'CREATE TABLE l7_2pc (id int primary key, v text)');
	my $tp_before = _counter('tt_2pc', 'twopc_prepare_records');

	$node->safe_psql('postgres', q{
		BEGIN; INSERT INTO l7_2pc VALUES (1, 'pc'); PREPARE TRANSACTION 'a226_c';
	});
	$node->safe_psql('postgres', q{
		BEGIN; INSERT INTO l7_2pc VALUES (2, 'pa'); PREPARE TRANSACTION 'a226_a';
	});
	$node->safe_psql('postgres', q{COMMIT PREPARED 'a226_c'});
	$node->safe_psql('postgres', q{ROLLBACK PREPARED 'a226_a'});

	_assert_mvcc_capability('L7_2pc_commit_prepared',
		q{SELECT v FROM l7_2pc WHERE id = 1}, 'pc');
	_assert_mvcc_capability('L7_2pc_rollback_prepared',
		q{SELECT count(*) FROM l7_2pc WHERE id = 2}, '0');
	my $tp_after = _counter('tt_2pc', 'twopc_prepare_records');
	ok($tp_after > $tp_before,
		"L7 twopc_prepare_records advanced ($tp_before -> $tp_after)");
}


# ============================================================
# L8: recovery — crash-restart redo idempotent, data intact.
# ============================================================
{
	my $before = $node->safe_psql('postgres',
		q{SELECT count(*), coalesce(sum(id), 0) FROM l1_undo});
	$node->stop('immediate');
	$node->start;
	my $after = $node->safe_psql('postgres',
		q{SELECT count(*), coalesce(sum(id), 0) FROM l1_undo});
	is($after, $before, 'L8 data intact across crash-restart redo');
	my $log = slurp_file($node->logfile);
	unlike($log, qr/PANIC/, 'L8 no PANIC during crash recovery');
	$report->record_mvcc_capability('L8_recovery_crash_restart',
		status => ($after eq $before) ? 'PASS' : 'FAIL', layer => 'hard');
}


# ============================================================
# L9: SCN monotone — own-instance scn_current_local advances.
# ============================================================
{
	my $scn1 = _counter('scn', 'scn_current_local');
	# A few committed writes advance the local SCN.
	$node->safe_psql('postgres', q{
		CREATE TABLE l9_scn (id int);
		INSERT INTO l9_scn SELECT generate_series(1, 20);
		UPDATE l9_scn SET id = id + 1000;
	});
	my $scn2 = _counter('scn', 'scn_current_local');
	ok($scn2 > $scn1, "L9 SCN monotone advance ($scn1 -> $scn2)");
	$report->record_mvcc_capability('L9_scn_monotone',
		status => ($scn2 > $scn1) ? 'PASS' : 'FAIL', layer => 'hard',
		counter_delta => ($scn2 - $scn1));
}


# ============================================================
# L10: metric matrix — all 6 MVCC dump categories emit rows.
# ============================================================
{
	my @cats = qw(undo cr tt_status visibility tt_2pc recovery);
	my $all = 1;
	for my $c (@cats)
	{
		my $n = $node->safe_psql('postgres',
			qq{SELECT count(*) FROM pg_cluster_state WHERE category='$c'});
		ok($n >= 1, "L10 dump category '$c' emits $n row(s)");
		$all = 0 if $n < 1;
	}
	$report->record_mvcc_capability('L10_metric_matrix',
		status => $all ? 'PASS' : 'FAIL', layer => 'hard',
		categories => join(',', @cats));
}


# Emit the acceptance report (best-effort — never fails the test).
{
	my $path = $report->default_path($ENV{TESTDIR} ? "$ENV{TESTDIR}/tmp" : 'tmp');
	eval { $report->emit_json($path); 1 }
	  and note("Stage 3 acceptance report -> $path");
}

$node->stop;
done_testing();
