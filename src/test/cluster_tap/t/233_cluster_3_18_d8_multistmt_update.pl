#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 233_cluster_3_18_d8_multistmt_update.pl
#	  spec-3.18 D8 -- clean-environment adjudicator for an intermittent
#	  multi-statement lost-update anomaly observed during D8 test authoring.
#
#	  Anomaly record (do NOT delete -- 規則 8.A / honesty):
#	    A cluster-mode single transaction that runs INSERT then UPDATE in ONE
#	    multi-statement simple-query string intermittently left some
#	    just-inserted rows un-updated (e.g. 480/500, 194/200, 176/200).  It
#	    reproduced 3x early on the macOS dev box, then 0/27 after thorough SysV
#	    IPC cleanup (ipcrm).  cluster.enabled = off was ALWAYS correct, and the
#	    full Stage 3 e2e suite (362 tests) is green on macOS + Linux CI.  The
#	    leading hypothesis is macOS-dev shared-memory/IPC instability, NOT a
#	    backend bug -- but that is NOT proven.
#
#	  This test is the adjudicator: it repeats the exact trigger N times and
#	  asserts ZERO lost updates.  Linux CI (and the fresh macOS CI runner) are
#	  the authoritative clean surfaces -- GREEN here classifies the anomaly as
#	  host-environment noise; a single RED is a real 規則 8.A correctness bug
#	  (a committed UPDATE not reflected in a later read == a false result) and
#	  MUST block ship.  Fails closed: no tolerance for any discrepancy.
#
#	  Author: SqlRush <sqlrush@gmail.com>
#	  Spec: spec-3.18-write-path-performance-overhaul.md (D8)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('s318_d8adj');
$node->init;
$node->append_conf('postgresql.conf', "cluster.enabled = on\n");
$node->append_conf('postgresql.conf', "cluster.node_id = 0\n");
$node->append_conf('postgresql.conf', "cluster.allow_single_node = on\n");
$node->append_conf('postgresql.conf', "cluster.undo_buffers = 64\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

my $N = 25;
my $rows = 200;
my @bad;

for my $k (1 .. $N) {
	my $t = "m$k";
	$node->safe_psql('postgres', "CREATE TABLE $t(id int primary key, w int)");
	# The exact trigger: INSERT then UPDATE of every just-inserted row in ONE
	# multi-statement simple-query string, one committed transaction.
	$node->safe_psql('postgres',
		    "BEGIN; "
		  . "INSERT INTO $t SELECT g, 0 FROM generate_series(1, $rows) g; "
		  . "UPDATE $t SET w = 9; "
		  . "COMMIT;");
	# Every row was inserted then updated in the same txn -> all must read w = 9.
	my $w9 = $node->safe_psql('postgres', "SELECT count(*) FROM $t WHERE w = 9");
	push @bad, "$t:$w9/$rows" if $w9 ne "$rows";
}

is(scalar(@bad), 0,
	"no lost updates across $N multi-statement INSERT+UPDATE txns"
	  . (@bad ? " [LOST: @bad]" : ""));

$node->stop;
done_testing();
