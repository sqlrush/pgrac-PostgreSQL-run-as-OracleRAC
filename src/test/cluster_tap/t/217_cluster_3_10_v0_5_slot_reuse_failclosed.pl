# 217_cluster_3_10_v0_5_slot_reuse_failclosed.pl
#
# spec-3.10 §v0.5 slot-reuse fail-closed end-to-end (E1 + E7).
#
#   E1:  force ITL slot reuse on a single heap block by committing more than
#        INITRANS (=8) distinct writers to that block, then drive a CR
#        construction with a read_scn older than the recycle -> the per-page
#        itl_recycle_watermark_scn is newer than read_scn -> construct fails
#        closed with 53R9F "ITL slot reused after snapshot" (never returns a
#        possibly false-visible CR image).
#   E7:  restart the node and re-run the same construct -> the watermark
#        survived WAL redo / checkpoint, so it still fails closed (proves the
#        guard is not lost across crash recovery; not FPI-dependent in unit
#        test_cluster_itl_reader_real_triple t32).
#
# Author: SqlRush <sqlrush@gmail.com>
# Spec: spec-3.10-cr-block-cache.md (§v0.5)

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('s310_v05');
$node->init;
$node->append_conf('postgresql.conf', "cluster.enabled = on\n");
$node->append_conf('postgresql.conf', "cluster.node_id = 0\n");
$node->append_conf('postgresql.conf', "cluster.allow_single_node = on\n");
$node->start;

# Narrow table so all rows live on heap block 0.
$node->safe_psql('postgres', 'CREATE TABLE t(id int, v int)');
$node->safe_psql('postgres',
	'INSERT INTO t SELECT g, 0 FROM generate_series(1, 80) g');

# Snapshot an SCN BEFORE any slot-reuse write; this is our "old reader".
my $scn_before = $node->safe_psql('postgres', 'SELECT cluster_scn_current()');
like($scn_before, qr/^\d+$/, "captured pre-write SCN ($scn_before)");

# Force > INITRANS(8) distinct committed writers on block 0 (each a separate
# connection => separate xid => its own ITL slot).  The 9th+ writer finds no
# FREE slot and recycles a COMMITTED data slot, stamping the recycle watermark
# with that evicted writer's write_scn (> $scn_before).
for my $i (1 .. 12)
{
	$node->safe_psql('postgres', "UPDATE t SET v = v + 1 WHERE id = $i");
}

# --- E1: construct with the old read_scn must fail closed (53R9F) -----------
my ($rc, $out, $err) = $node->psql('postgres',
	"SELECT cluster_cr_test_construct('t'::regclass, 0, 0, $scn_before)");
isnt($rc, 0, 'E1: CR construct with pre-reuse read_scn errors');
like($err, qr/ITL slot reused after snapshot/,
	'E1: fail-closed message is the §v0.5 slot-reuse guard (53R9F)');
# (default psql does not print the SQLSTATE; the distinct errmsg above
#  uniquely identifies the §v0.5 ERRCODE_CLUSTER_CR_SNAPSHOT_TOO_OLD path.)

# Negative control: a maximal read_scn is never older than the watermark, so
# the slot-reuse guard must NOT fire (it may still fail for unrelated reasons,
# but never with the slot-reuse message).
my (undef, undef, $err_max) = $node->psql('postgres',
	"SELECT cluster_cr_test_construct('t'::regclass, 0, 0, 9223372036854775807)");
unlike($err_max // '', qr/ITL slot reused after snapshot/,
	'control: max read_scn does not trip the slot-reuse guard');

# --- E7: watermark survives restart (WAL redo / checkpoint) -----------------
$node->safe_psql('postgres', 'CHECKPOINT');
$node->restart;

my (undef, undef, $err_after) = $node->psql('postgres',
	"SELECT cluster_cr_test_construct('t'::regclass, 0, 0, $scn_before)");
like($err_after, qr/ITL slot reused after snapshot/,
	'E7: watermark persisted across restart -> still fails closed');

$node->stop;
done_testing();
