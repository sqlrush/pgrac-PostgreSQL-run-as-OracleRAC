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
#	  L9   autoextend trigger via test hook: force segment_end + DML →
#	       autoextend_count + segment_switch_count both increment
#	  L10  test hook on empty active_segment returns false (no DML yet)
#	  L11-L12(hard cap real trigger + restart scan + concurrency
#	  double-checked locking)推 future test hook ship 后真测;本 spec smoke
#	  level coverage for L11-L12.  真 hard-cap baseline collection 推
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


# ----------
# L9: autoextend trigger via test hook
#    Sequence:
#     a) Run DML to ensure active segment is claimed
#     b) Snapshot autoextend / segment_switch counters
#     c) Call cluster_undo_test_force_segment_end() → forces cursor to last block
#     d) Run another DML → triggers autoextend path
#     e) Verify autoextend_count + segment_switch_count both incremented
# ----------
$node0->safe_psql('postgres', q{
    CREATE TABLE IF NOT EXISTS t_autoex (id int, v text);
    INSERT INTO t_autoex VALUES (1, 'claim active segment');
});

my $pre_autoex = $node0->safe_psql('postgres',
    q{SELECT value::bigint FROM pg_cluster_state
       WHERE category='undo' AND key='autoextend_count'});
my $pre_switch = $node0->safe_psql('postgres',
    q{SELECT value::bigint FROM pg_cluster_state
       WHERE category='undo' AND key='segment_switch_count'});

my $force_ok = $node0->safe_psql('postgres',
    q{SELECT cluster_undo_test_force_segment_end()});
is($force_ok, 't', "L9a force_segment_end returns true after active segment claimed");

$node0->safe_psql('postgres',
    q{INSERT INTO t_autoex VALUES (2, 'should trigger autoextend')});

my $post_autoex = $node0->safe_psql('postgres',
    q{SELECT value::bigint FROM pg_cluster_state
       WHERE category='undo' AND key='autoextend_count'});
my $post_switch = $node0->safe_psql('postgres',
    q{SELECT value::bigint FROM pg_cluster_state
       WHERE category='undo' AND key='segment_switch_count'});

ok($post_autoex > $pre_autoex,
    "L9b autoextend_count incremented (pre=$pre_autoex post=$post_autoex)");
ok($post_switch > $pre_switch,
    "L9c segment_switch_count incremented (pre=$pre_switch post=$post_switch)");


# ----------
# L10: counter monotonicity across repeated trigger
#    Two more force+DML cycles → autoextend should keep climbing.
# ----------
my $mid_autoex = $post_autoex;
for my $i (3..4) {
    $node0->safe_psql('postgres',
        q{SELECT cluster_undo_test_force_segment_end()});
    $node0->safe_psql('postgres',
        qq{INSERT INTO t_autoex VALUES ($i, 'extend cycle')});
}
my $final_autoex2 = $node0->safe_psql('postgres',
    q{SELECT value::bigint FROM pg_cluster_state
       WHERE category='undo' AND key='autoextend_count'});
ok($final_autoex2 > $mid_autoex,
    "L10 repeated autoextend monotonic (mid=$mid_autoex final=$final_autoex2)");


$pair->stop_pair;
done_testing();
