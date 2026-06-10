#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 241_cluster_3_25_undo_wal_bundle.pl
#    spec-3.25 D1b -- deferred per-(xact,block) undo WAL merge.
#
#      L1  bundle factor: one multi-row txn bumps record_alloc_count by N but
#          block_write_count (now per-EMISSION) by ~blocks-touched (<< N).
#      L2  the merged record form appears in WAL (UNDO_BLOCK_WRITE_MULTI),
#          delta form included (same-backend later txn re-touches the block).
#      L3  commit + crash BEFORE write-back: redo of the merged record
#          reconstructs the undo block; a held snapshot still CR-reads the
#          pre-update value through the recovered chain (8.A CR-available).
#      L4  abort parity: an aborted txn's merged record is still emitted
#          (same as the old per-record behaviour) -- no PANIC, clean restart.
#      L5  crash MID-txn (pending never flushed): consistent restart, the
#          uncommitted rows are simply absent, no undo corruption.
#      L6  write-through (writeback=off) keeps the per-record 1:1 emission
#          (no deferral on that path).
#
#    Author: SqlRush <sqlrush@gmail.com>
#    Spec: spec-3.25-oracle-style-wal-undo-structural-tax.md (D1b / §5.11)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PgracClusterNode;
use PostgreSQL::Test::Utils;
use Test::More;

sub counter
{
	my ($node, $cat, $key) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT COALESCE((SELECT value::bigint FROM pg_cluster_state
			WHERE category='$cat' AND key='$key'), 0)});
}

my $node = PgracClusterNode->new('undo_bundle');
$node->init;
$node->append_conf('postgresql.conf',
	    "cluster.enabled = on\n"
	  . "cluster.node_id = 0\n"
	  . "cluster.allow_single_node = on\n"
	  . "cluster.undo_buffers = 64\n"
	  . "cluster.undo_buffer_writeback = on\n"
	  . "checkpoint_timeout = 1h\n"
	  . "max_wal_size = 4GB\n"
	  . "autovacuum = off\n");
$node->start;

$node->safe_psql('postgres',
	'CREATE TABLE t325 (id int primary key, v text)');

# ============================================================
# L1: bundle factor -- N undo records, ~1 emission per touched block.
# ============================================================
my $alloc0 = counter($node, 'undo', 'record_alloc_count');
my $emit0  = counter($node, 'undo', 'block_write_count');

$node->safe_psql('postgres',
	q{INSERT INTO t325 SELECT g, 'v'||g FROM generate_series(1, 200) g});

my $alloc_d = counter($node, 'undo', 'record_alloc_count') - $alloc0;
my $emit_d  = counter($node, 'undo', 'block_write_count') - $emit0;

cmp_ok($alloc_d, '>=', 200, "L1 INSERT emitted >=200 undo records ($alloc_d)");
cmp_ok($emit_d, '>', 0, "L1 at least one merged emission ($emit_d)");
cmp_ok($emit_d * 4, '<', $alloc_d,
	"L1 bundle factor >4 (records=$alloc_d, emissions=$emit_d)");

# ============================================================
# L2: merged record (+ delta form) visible in WAL.
# ============================================================
my $bg = $node->background_psql('postgres');
$bg->query_safe(q{UPDATE t325 SET v = v||'_a' WHERE id <= 5});
$bg->query_safe(q{UPDATE t325 SET v = v||'_b' WHERE id <= 5});
$bg->quit;

my ($first_seg) = sort glob($node->data_dir . '/pg_wal/0*');
my ($wd, $wderr) = run_command(
	[ $node->installed_command('pg_waldump'), '-r', 'ClusterUndo', $first_seg ]);
my $n_multi = () = $wd =~ /UNDO_BLOCK_WRITE_MULTI/g;
my $n_multi_delta = () = $wd =~ /UNDO_BLOCK_WRITE_MULTI.*\(3-span multi delta\)/g;
cmp_ok($n_multi, '>', 0, "L2 merged records in WAL (n=$n_multi)");
cmp_ok($n_multi_delta, '>', 0, "L2 delta-form merged records in WAL (n=$n_multi_delta)");

# ============================================================
# L3: commit + crash before write-back -> merged-record redo + CR intact.
# ============================================================
$node->safe_psql('postgres', q{UPDATE t325 SET v = 'pre' WHERE id = 7});
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', q{UPDATE t325 SET v = 'post' WHERE id = 7});
$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', q{SELECT v FROM t325 WHERE id = 7}),
	'post', 'L3 committed row survives crash-before-writeback');

my $s1 = $node->background_psql('postgres', on_error_die => 1);
$s1->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $seen0 = $s1->query_safe('SELECT v FROM t325 WHERE id = 7');
$node->safe_psql('postgres', q{UPDATE t325 SET v = 'newer' WHERE id = 7});
my $seen1 = $s1->query_safe('SELECT v FROM t325 WHERE id = 7');
$s1->query_safe('COMMIT');
$s1->quit;
is($seen0, 'post', 'L3 RR snapshot baseline');
is($seen1, 'post',
	'L3 CR through the crash-recovered undo chain still serves the old value');
unlike(slurp_file($node->logfile), qr/PANIC/, 'L3 no PANIC');

# ============================================================
# L4: abort parity -- merged record emitted on ABORT too; clean restart.
# ============================================================
my $emit_a0 = counter($node, 'undo', 'block_write_count');
my $ab = $node->background_psql('postgres');
$ab->query_safe('BEGIN');
$ab->query_safe(q{UPDATE t325 SET v = v||'_dead' WHERE id <= 20});
$ab->query_safe('ROLLBACK');
$ab->quit;
my $emit_a1 = counter($node, 'undo', 'block_write_count');
cmp_ok($emit_a1, '>', $emit_a0, 'L4 aborted txn still emitted its merged record');
is($node->safe_psql('postgres', q{SELECT count(*) FROM t325 WHERE v LIKE '%_dead'}),
	'0', 'L4 aborted rows invisible');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{SELECT count(*) FROM t325}),
	'200', 'L4 clean restart after abort-emitted merged record');

# ============================================================
# L5: crash MID-txn -- pending never flushed; consistent restart.
# ============================================================
my $mid = $node->background_psql('postgres');
$mid->query_safe('BEGIN');
$mid->query_safe(q{INSERT INTO t325 SELECT g, 'mid'||g FROM generate_series(900, 940) g});
# Crash with the txn (and its pending merged record) still open.
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{SELECT count(*) FROM t325 WHERE id >= 900}),
	'0', 'L5 uncommitted rows absent after mid-txn crash');
is($node->safe_psql('postgres', q{SELECT count(*) FROM t325}),
	'200', 'L5 table consistent after mid-txn crash (pending undo never WAL\'d)');
unlike(slurp_file($node->logfile), qr/PANIC/, 'L5 no PANIC');

# ============================================================
# L6: write-through path keeps per-record 1:1 emission.
# ============================================================
$node->safe_psql('postgres',
	q{ALTER SYSTEM SET cluster.undo_buffer_writeback = off});
$node->safe_psql('postgres', q{SELECT pg_reload_conf()});

my $alloc_w0 = counter($node, 'undo', 'record_alloc_count');
my $emit_w0  = counter($node, 'undo', 'block_write_count');
$node->safe_psql('postgres',
	q{INSERT INTO t325 SELECT g, 'wt'||g FROM generate_series(500, 549) g});
my $alloc_wd = counter($node, 'undo', 'record_alloc_count') - $alloc_w0;
my $emit_wd  = counter($node, 'undo', 'block_write_count') - $emit_w0;
cmp_ok($alloc_wd, '>=', 50, "L6 write-through emitted >=50 undo records ($alloc_wd)");
is($emit_wd, $alloc_wd,
	"L6 write-through stays 1:1 per-record ($emit_wd == $alloc_wd)");

done_testing();
