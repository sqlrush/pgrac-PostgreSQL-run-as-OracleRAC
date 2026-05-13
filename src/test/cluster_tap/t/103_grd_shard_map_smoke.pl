#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 103_grd_shard_map_smoke.pl
#	  spec-2.14 D14: single-node TAP smoke for GRD routing substrate.
#
#	  Verifies the spec-2.14 routing layer end-to-end via SQL surface:
#	    (a) pg_cluster_grd_shards view returns 4096 rows (PGRAC_GRD_SHARD_COUNT)
#	    (b) single-node cluster: all shards mastered by self (DISTINCT = 1)
#	    (c) is_local = true for every row (single node = all local)
#	    (d) deterministic ordering + master assignment
#
#	  v0.4 P2 修正:  TAP 103 单节点 mandatory only,no IC ClusterPair
#	  dependency.  recent L66 family fix (spec-2.13 v1.0.2) 稳定 tier1
#	  但 spec-2.14 是本地 routing substrate,不应把 stable tier1 layer
#	  拉进 spec-2.14 风险面.  2-node mapping consistency + sparse node_id
#	  coverage 由 unit test T-grd-1d sparse-node mock 替代 (unit 比 TAP
#	  适合 algorithm-level verification — 不需 postmaster + 可控
#	  declared list + deterministic).
#
#	  Spec authority:  pgrac:specs/spec-2.14-grd-resource-identity-
#	  shard-routing.md (frozen v0.4 Q1-Q15 2026-05-13).
#
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
# Portions Copyright (c) 2026, pgrac contributors
#
# Author: SqlRush <sqlrush@gmail.com>
#
# IDENTIFICATION
#	  src/test/cluster_tap/t/103_grd_shard_map_smoke.pl
#
# NOTES
#	  pgrac-original file.
#	  Spec: spec-2.14-grd-resource-identity-shard-routing.md (frozen v0.4)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PgracClusterNode;


# ----------
# L1:  single-node primary init + start (no ClusterPair / no IC).
# ----------
my $node = PgracClusterNode->new('main');
$node->init;
$node->append_conf('postgresql.conf', "cluster.node_id = 0\n");
$node->start;

is($node->safe_psql('postgres', 'SELECT 1'), '1',
   'L1 single-node primary alive');


# ----------
# L2:  pg_cluster_grd_shards returns 4096 rows (PGRAC_GRD_SHARD_COUNT).
# ----------
my $shard_count = $node->safe_psql('postgres',
	'SELECT count(*)::int FROM pg_cluster_grd_shards');
is($shard_count, '4096',
   'L2 pg_cluster_grd_shards returns 4096 rows (PGRAC_GRD_SHARD_COUNT)');


# ----------
# L3:  single-node cluster — all shards mastered by self (DISTINCT = 1).
# ----------
my $distinct_masters = $node->safe_psql('postgres',
	'SELECT count(DISTINCT master_node_id)::int FROM pg_cluster_grd_shards');
is($distinct_masters, '1',
   'L3 single-node cluster: 1 distinct master_node_id (self)');

my $self_master = $node->safe_psql('postgres',
	'SELECT DISTINCT master_node_id FROM pg_cluster_grd_shards');
is($self_master, '0',
   'L3a master_node_id = cluster.node_id (0) for all shards');


# ----------
# L4:  is_local = true for every row (single node).
# ----------
my $all_local = $node->safe_psql('postgres',
	'SELECT count(*)::int FROM pg_cluster_grd_shards WHERE is_local');
is($all_local, '4096',
   'L4 all 4096 shards marked is_local = true (single-node self master)');


# ----------
# L5:  cleanup.
# ----------
$node->stop;

done_testing();
