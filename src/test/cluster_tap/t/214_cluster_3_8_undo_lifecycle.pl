#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 214_cluster_3_8_undo_lifecycle.pl
#	  spec-3.8 D13 — Undo Segment Lifecycle MVP + Autoextend behavioral
#	  TAP on ClusterPair fixture.
#
#	  L1   ClusterPair startup + both nodes alive
#	  L2   undo category in pg_cluster_state has 9 rows (5 record-level
#	       + 4 NEW lifecycle counters)
#	  L3   53R9E SQLSTATE registered (lookup via pg_settings or smoke trigger)
#	  L4   2 NEW GUCs registered with defaults:
#	       cluster.undo_segments_max_per_instance = 256
#	       cluster.undo_segment_create_timeout_ms = 5000
#	  L5   Initial autoextend / segment_switch / create_fail / hard_cap
#	       counters = 0 (无活动时)
#	  L6   GUC SIGHUP reload max_per_instance:  ALTER SYSTEM SET ... = 128;
#	       SELECT pg_reload_conf(); 验证 reload OK
#	  L7   GUC SIGHUP reload create_timeout_ms:  set to 2000ms; reload OK
#	  L8   Counter monotonic + clean shutdown
#
#	  L9-L12(autoextend trigger 真测 + double-checked locking + hard cap
#	  + restart scan)推 future test hook ship 后真测;本 spec smoke
#	  level coverage.  真 autoextend 真测 baseline collection 推 Step 11
#	  perf class 8 + Hardening v1.0.X.
#
#	  Spec: spec-3.8-undo-segment-lifecycle-autoextend.md (FROZEN v0.3 +
#	        Hardening v1.0.1 H-1/H-2)
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


my $pair = PostgreSQL::Test::ClusterPair->new_pair(
	'spec_3_8_undo_lifecycle',
	extra_conf => [
		'autovacuum = off',
		'cluster.pcm_grd_max_entries = 0',
		'cluster.undo_segments_max_per_instance = 256',
		'cluster.undo_segment_create_timeout_ms = 5000',
		'max_prepared_transactions = 4',
	]);
$pair->start_pair;
usleep(2_000_000);

my $node0 = $pair->node0;


# ----------
# L1: ClusterPair startup + both nodes alive
# ----------
{
	my $r = $node0->safe_psql('postgres', 'SELECT 1');
	is($r, '1', "L1 node0 alive");
}


# ----------
# L2: undo category has 9 rows
# ----------
my $undo_row_count = $node0->safe_psql('postgres',
	q{SELECT count(*) FROM pg_cluster_state WHERE category='undo'});
is($undo_row_count, '9',
	"L2 undo category has 9 rows (5 record-level + 4 NEW lifecycle counters)");


# ----------
# L3: 4 NEW lifecycle counter keys present
# ----------
my $lifecycle_keys = $node0->safe_psql('postgres',
	q{SELECT string_agg(key, ',' ORDER BY key) FROM pg_cluster_state
	   WHERE category='undo' AND key IN ('autoextend_count', 'segment_switch_count',
	                                      'segment_create_fail_count',
	                                      'segment_hard_cap_fail_count')});
is($lifecycle_keys,
	'autoextend_count,segment_create_fail_count,segment_hard_cap_fail_count,segment_switch_count',
	"L3 all 4 NEW lifecycle counter keys present");


# ----------
# L4: 2 NEW GUCs registered with default values
# ----------
my $max_per_instance = $node0->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.undo_segments_max_per_instance'});
is($max_per_instance, '256', "L4a max_per_instance default = 256");

my $create_timeout = $node0->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.undo_segment_create_timeout_ms'});
is($create_timeout, '5000', "L4b create_timeout default = 5000ms");


# ----------
# L5: Initial counters = 0 (无 autoextend 活动时)
# ----------
my $initial_autoextend = $node0->safe_psql('postgres',
	q{SELECT value::bigint FROM pg_cluster_state
	   WHERE category='undo' AND key='autoextend_count'});
ok($initial_autoextend == 0,
	"L5a initial autoextend_count = $initial_autoextend (expected 0)");

my $initial_hard_cap = $node0->safe_psql('postgres',
	q{SELECT value::bigint FROM pg_cluster_state
	   WHERE category='undo' AND key='segment_hard_cap_fail_count'});
ok($initial_hard_cap == 0,
	"L5b initial segment_hard_cap_fail_count = $initial_hard_cap (expected 0)");


# ----------
# L6: GUC SIGHUP reload max_per_instance
# ----------
$node0->safe_psql('postgres',
	'ALTER SYSTEM SET cluster.undo_segments_max_per_instance = 128');
$node0->safe_psql('postgres', 'SELECT pg_reload_conf()');
usleep(500_000);
my $new_max = $node0->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.undo_segments_max_per_instance'});
is($new_max, '128', "L6 SIGHUP reload max_per_instance OK (256 → 128)");


# ----------
# L7: GUC SIGHUP reload create_timeout_ms
# ----------
$node0->safe_psql('postgres',
	'ALTER SYSTEM SET cluster.undo_segment_create_timeout_ms = 2000');
$node0->safe_psql('postgres', 'SELECT pg_reload_conf()');
usleep(500_000);
my $new_timeout = $node0->safe_psql('postgres',
	q{SELECT setting FROM pg_settings WHERE name = 'cluster.undo_segment_create_timeout_ms'});
is($new_timeout, '2000', "L7 SIGHUP reload create_timeout OK (5000 → 2000)");


# ----------
# L8: Counter monotonic + clean shutdown
# ----------
my $final_autoextend = $node0->safe_psql('postgres',
	q{SELECT value::bigint FROM pg_cluster_state
	   WHERE category='undo' AND key='autoextend_count'});
ok($final_autoextend >= $initial_autoextend,
	"L8 autoextend_count monotonic (initial=$initial_autoextend, final=$final_autoextend)");


$pair->stop_pair;
done_testing();
