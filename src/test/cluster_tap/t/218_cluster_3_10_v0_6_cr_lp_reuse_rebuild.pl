#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 218_cluster_3_10_v0_6_cr_lp_reuse_rebuild.pl
#	  spec-3.10 §v0.6 — CR block reconstruction robust to PG line-pointer
#	  reuse (规则 8.A P0).
#
#	  Root cause: PG's prune horizon (OldestXmin / heap_page_prune) is
#	  decoupled from the cluster CR read_scn (AD-012).  A committed-dead old
#	  version below OldestXmin is freed by VACUUM and its line pointer reused
#	  by a later INSERT, yet a reader with an older read_scn still needs that
#	  old image.  The pre-fix memcpy(live)+inverse-apply path then hits a
#	  different-length or already-freed offset → `cluster CR update/delete
#	  inverse-apply failed`.  §v0.6 reorders to prune-first + offset-aware
#	  variable-length-safe re-add so the read_scn image is rebuilt correctly.
#
#	  Content (not just rc==0) is asserted via the §v0.6 SETOF-record SRF
#	  cluster_cr_test_image(rel, blockno, read_scn) AS r(cr_off int2, ...):
#	    P1  different-length line-pointer reuse → rebuild succeeds, image is
#	        exactly the read_scn rows (no id4, no post-snapshot new value)
#	    P2  same-length line-pointer reuse → same
#	    P3  multi-row / multi-candidate (two UPDATEs + two reusing INSERTs)
#	    P4  DELETE + reuse → deleted-at-read_scn row restored
#	    P5  controls: normal in-place CR still works after the reorder; a
#	        max read_scn (no rollback) returns the live image incl. new value
#
#	  Single-node (cluster.allow_single_node = on); the test SRF drives the
#	  read_scn path directly, which is the faithful stand-in for a reader
#	  whose read_scn is older than OldestXmin (cross-node / SCN snapshot).
#
#	  Spec: spec-3.10-cr-block-cache.md (§v0.6 FROZEN 2026-06-02)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $MAXSCN = '9223372036854775807';

my $node = PostgreSQL::Test::Cluster->new('s310_v06');
$node->init;
$node->append_conf('postgresql.conf', "cluster.enabled = on\n");
$node->append_conf('postgresql.conf', "cluster.node_id = 0\n");
$node->append_conf('postgresql.conf', "cluster.allow_single_node = on\n");
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

# image($tbl, $scn) -> "id:v,id:v,..." ordered by id, from the rebuilt CR image.
my $image = sub {
	my ($tbl, $scn) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT coalesce(string_agg(id||':'||v, ',' ORDER BY id), '(empty)')
		  FROM cluster_cr_test_image('$tbl'::regclass, 0, $scn)
		       AS r(cr_off int2, id int, v text)});
};
# count_v($tbl,$scn,$txt) -> # of CR-image rows whose v equals $txt.
my $count_v = sub {
	my ($tbl, $scn, $txt) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT count(*) FROM cluster_cr_test_image('$tbl'::regclass, 0, $scn)
		       AS r(cr_off int2, id int, v text) WHERE v = '$txt'});
};
# cr_off_of($tbl,$scn,$id) -> the rebuilt offnum carrying row $id.
my $cr_off_of = sub {
	my ($tbl, $scn, $id) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT coalesce(min(cr_off)::text,'(none)')
		  FROM cluster_cr_test_image('$tbl'::regclass, 0, $scn)
		       AS r(cr_off int2, id int, v text) WHERE id = $id});
};
# construct_rc($tbl,$scn) -> psql rc (0 = construction did not error).
my $construct_rc = sub {
	my ($tbl, $scn) = @_;
	my ($rc, undef, undef) = $node->psql('postgres',
		"SELECT cluster_cr_test_construct('$tbl'::regclass, 0, 0, $scn)");
	return $rc;
};

# Build a 3-row varlen table on block 0, capture a pre-write read_scn, then
# run $mutate (UPDATE/DELETE making old versions dead) + VACUUM (frees their
# line pointers) + $reuse (INSERTs that reuse the freed offsets).  Returns scn0.
my $build = sub {
	my ($tbl, $mutate, $reuse) = @_;
	$node->safe_psql('postgres', "CREATE TABLE $tbl(id int, v text)");
	$node->safe_psql('postgres',
		"INSERT INTO $tbl VALUES (1,repeat('A',5)),(2,repeat('B',5)),(3,repeat('C',5))");
	my $scn0 = $node->safe_psql('postgres', 'SELECT cluster_scn_current()');
	$node->safe_psql('postgres', $mutate);
	$node->safe_psql('postgres', "VACUUM $tbl");
	$node->safe_psql('postgres', $reuse) if $reuse;
	return $scn0;
};

my $READ = '1:AAAAA,2:BBBBB,3:CCCCC';    # the read_scn-state of every table

# ---- P1: different-length line-pointer reuse -------------------------------
{
	my $s = $build->('t_p1',
		q{UPDATE t_p1 SET v=repeat('Z',40) WHERE id=1},
		q{INSERT INTO t_p1 VALUES (4,repeat('Q',12))});
	is($construct_rc->('t_p1', $s), 0, 'P1a rebuild succeeds (diff-length reuse)');
	is($image->('t_p1', $s), $READ, 'P1b CR image is exactly the read_scn rows');
	is($count_v->('t_p1', $s, ('Z' x 40)), '0', 'P1c post-snapshot new value absent');
	is($count_v->('t_p1', $s, ('Q' x 12)), '0', 'P1d reusing-INSERT row absent');
	is($cr_off_of->('t_p1', $s, 1), '1', 'P1e id=1 restored at its read_scn offnum 1');
}

# ---- P2: same-length line-pointer reuse -----------------------------------
{
	my $s = $build->('t_p2',
		q{UPDATE t_p2 SET v=repeat('Z',40) WHERE id=1},
		q{INSERT INTO t_p2 VALUES (4,repeat('Q',5))});    # 5 == len('AAAAA')
	is($construct_rc->('t_p2', $s), 0, 'P2a rebuild succeeds (same-length reuse)');
	is($image->('t_p2', $s), $READ, 'P2b CR image is exactly the read_scn rows');
	is($count_v->('t_p2', $s, ('Q' x 5)), '0', 'P2c same-length reuser row absent');
}

# ---- P3: multi-row / multi-candidate (two UPDATEs + two reusing INSERTs) ---
{
	my $s = $build->('t_p3',
		q{UPDATE t_p3 SET v=repeat('Z',40) WHERE id IN (1,2)},
		q{INSERT INTO t_p3 VALUES (4,repeat('Q',9)),(5,repeat('R',13))});
	is($construct_rc->('t_p3', $s), 0, 'P3a rebuild succeeds (multi-candidate)');
	is($image->('t_p3', $s), $READ, 'P3b CR image is exactly the read_scn rows');
	is($count_v->('t_p3', $s, ('Z' x 40)), '0', 'P3c neither new version present');
}

# ---- P4: DELETE + reuse ---------------------------------------------------
{
	my $s = $build->('t_p4',
		q{DELETE FROM t_p4 WHERE id=1},
		q{INSERT INTO t_p4 VALUES (4,repeat('Q',12))});
	is($construct_rc->('t_p4', $s), 0, 'P4a rebuild succeeds (DELETE + reuse)');
	is($image->('t_p4', $s), $READ, 'P4b deleted-at-read_scn row id=1 restored');
	is($cr_off_of->('t_p4', $s, 1), '1', 'P4c id=1 restored at read_scn offnum 1');
}

# ---- P5: controls — normal in-place CR + max read_scn = live image --------
{
	# No VACUUM, no reuse: the old id=1 version stays in place → the normal
	# length-preserving inverse-update path (must keep working after reorder).
	$node->safe_psql('postgres', 'CREATE TABLE t_p5(id int, v text)');
	$node->safe_psql('postgres',
		"INSERT INTO t_p5 VALUES (1,repeat('A',5)),(2,repeat('B',5)),(3,repeat('C',5))");
	my $s = $node->safe_psql('postgres', 'SELECT cluster_scn_current()');
	$node->safe_psql('postgres', q{UPDATE t_p5 SET v=repeat('Z',40) WHERE id=1});

	is($construct_rc->('t_p5', $s), 0, 'P5a normal in-place CR still succeeds');
	is($image->('t_p5', $s), $READ, 'P5b read_scn image rolls back the in-place UPDATE');
	is($count_v->('t_p5', $s, ('Z' x 40)), '0', 'P5c new value absent at read_scn');
	# Max read_scn: nothing is newer → no rollback → the live image, which
	# DOES contain the post-snapshot new value (proves the control direction).
	is($count_v->('t_p5', $MAXSCN, ('Z' x 40)), '1',
		'P5d max read_scn returns the live image incl. the new value');
}

$node->stop;
done_testing();
