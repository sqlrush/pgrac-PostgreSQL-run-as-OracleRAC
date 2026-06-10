#!/usr/bin/env perl
#-------------------------------------------------------------------------
#
# 244_wal_state_registry.pl
#    spec-4.2 -- ClusterWalState registry, single-node surface.
#
#      L1   first boot creates <root>/pgrac_wal_state (66048 bytes);
#           registry_ready=t; own slot reaches 'active' only after the
#           node serves (phase -> RUNNING publish)
#      L2   cluster_stats refresh advances registry_last_updated and
#           registry_highest_lsn while the node runs
#      L3   clean stop publishes STOPPED (verified via raw slot bytes
#           and via the dump key after restart)
#      L4   kill -9 leaves the slot ACTIVE (crash never writes STOPPED;
#           restart re-publishes ACTIVE with a new started_at)
#      L5   own-slot corruption self-heals: the next ACTIVE publish
#           overwrites the torn slot (owner repairs its own slot)
#      L6   header corruption -> startup FATAL 53RA2 (never rebuilt
#           automatically); restoring the header recovers
#      L7   chmod-based publish failure -> startup FATAL 53RA2 (the
#           registered injection point cannot be armed before first
#           boot; same real-fault pattern as t/242 L11)
#      L8   foreign/corrupt NEIGHBOUR slots never block this node and
#           surface as their own slots only (no cross-slot bleed)
#      L9   wal_threads_dir unset -> registry_ready=f, no file
#      L9b  recovery failure does not publish ACTIVE: a mis-linked
#           pg_wal FATALs in 4.1 validation (before recovery) and the
#           slot keeps its previous content
#      L10  dump keys 10/10 under wal_thread; wait events 2/2
#
#    Author: SqlRush <sqlrush@gmail.com>
#    Spec: spec-4.2-wal-thread-metadata-catalog.md (FROZEN v1.0)
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use File::Path qw(make_path);
use PgracClusterNode;
use PostgreSQL::Test::Utils;
use Test::More;

sub dumpkey
{
	my ($node, $key) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT value FROM pg_cluster_state
		WHERE category='wal_thread' AND key='$key'});
}

# Raw slot peek: offset 512 + (tid-1)*512, unpack a few fixed fields.
sub read_slot_raw
{
	my ($regfile, $tid) = @_;
	open my $fh, '<:raw', $regfile or die "open $regfile: $!";
	sysseek($fh, 512 + ($tid - 1) * 512, 0) or die "seek: $!";
	sysread($fh, my $buf, 512) == 512 or die "short read";
	close $fh;
	my ($magic, $version, $thread_id, $node_id, $state) = unpack('LSSlL', $buf);
	my ($started_at) = unpack('q', substr($buf, 24, 8));
	return { magic => $magic, thread_id => $thread_id, node_id => $node_id,
		state => $state, started_at => $started_at };
}

sub patch_byte
{
	my ($path, $off) = @_;
	open my $fh, '+<:raw', $path or die "open $path: $!";
	sysseek($fh, $off, 0) or die "seek: $!";
	sysread($fh, my $old, 1) or die "read: $!";
	sysseek($fh, $off, 0) or die "seek: $!";
	syswrite($fh, chr(ord($old) ^ 0xFF), 1) or die "write: $!";
	close $fh;
}

my $wroot = PostgreSQL::Test::Utils::tempdir();
my $regfile = "$wroot/pgrac_wal_state";

my $node = PgracClusterNode->new('wal_state_a');
$node->init(extra => [ '-X', "$wroot/thread_4" ]);
$node->append_conf('postgresql.conf',
	    "cluster.enabled = on\n"
	  . "cluster.node_id = 3\n"
	  . "cluster.allow_single_node = on\n"
	  . "cluster.wal_threads_dir = '$wroot'\n"
	  . "cluster.cluster_stats_main_loop_interval = '500ms'\n"
	  . "autovacuum = off\n");
$node->start;

# ============================================================
# L1: registry created; ACTIVE published at the RUNNING transition.
# ============================================================
ok(-f $regfile, 'L1 registry file exists after first boot');
is(-s $regfile, 66048, 'L1 registry file is the fixed 66048 bytes');
is(dumpkey($node, 'registry_ready'), 't', 'L1 registry_ready');
is(dumpkey($node, 'registry_slot_state'), 'active',
	'L1 own slot is ACTIVE once the node serves SQL');
{
	my $slot = read_slot_raw($regfile, 4);
	is($slot->{thread_id}, 4, 'L1 slot self-describes thread 4');
	is($slot->{node_id},   3, 'L1 slot records node_id 3');
	is($slot->{state},     1, 'L1 raw state == ACTIVE(1)');
}

# ============================================================
# L2: stats tick refreshes liveness stamp + watermarks.
# ============================================================
my $ts0  = dumpkey($node, 'registry_last_updated');
my $lsn0 = dumpkey($node, 'registry_highest_lsn');
$node->safe_psql('postgres',
	q{CREATE TABLE t244 AS SELECT g FROM generate_series(1, 2000) g});
my $deadline = time() + 15;
my ($ts1, $lsn1) = ($ts0, $lsn0);
while (time() < $deadline) {
	$ts1  = dumpkey($node, 'registry_last_updated');
	$lsn1 = dumpkey($node, 'registry_highest_lsn');
	last if $ts1 ne $ts0 && $lsn1 ne $lsn0;
	select(undef, undef, undef, 0.25);
}
cmp_ok($ts1, '>', $ts0, "L2 registry_last_updated advances ($ts0 -> $ts1)");
isnt($lsn1, $lsn0, 'L2 registry_highest_lsn advances with WAL volume');

# ============================================================
# L3: clean stop publishes STOPPED.
# ============================================================
$node->stop;    # fast = clean
{
	my $slot = read_slot_raw($regfile, 4);
	is($slot->{state}, 2, 'L3 raw state == STOPPED(2) after clean stop');
}
$node->start;
is(dumpkey($node, 'registry_slot_state'), 'active',
	'L3 restart republishes ACTIVE');

# ============================================================
# L4: crash leaves ACTIVE; restart re-publishes with new started_at.
# ============================================================
my $started_before = read_slot_raw($regfile, 4)->{started_at};
$node->stop('immediate');
{
	my $slot = read_slot_raw($regfile, 4);
	is($slot->{state}, 1, 'L4 immediate shutdown leaves the slot ACTIVE');
	is($slot->{started_at}, $started_before,
		'L4 crash did not rewrite the slot (same incarnation stamp)');
}
$node->start;
{
	my $slot = read_slot_raw($regfile, 4);
	is($slot->{state}, 1, 'L4 restart publishes ACTIVE again');
	cmp_ok($slot->{started_at}, '>', $started_before,
		'L4 new incarnation has a newer started_at');
}

# ============================================================
# L5: own-slot corruption self-heals on the next publish.
# ============================================================
$node->stop;
# slot 4 starts at 512 + (4-1)*512 = 2048; flip one body byte -> bad CRC
patch_byte($regfile, 2048 + 41);
$node->start;
is(dumpkey($node, 'registry_slot_state'), 'active',
	'L5 corrupt own slot overwritten by the ACTIVE publish (owner self-heal)');

# ============================================================
# L6: header corruption -> FATAL 53RA2, never auto-rebuilt.
# ============================================================
$node->stop;
my $hdr_orig;
{
	open my $fh, '<:raw', $regfile or die;
	sysread($fh, $hdr_orig, 512) == 512 or die;
	close $fh;
}
patch_byte($regfile, 0);    # magic
my $log_off = -s $node->logfile;
is($node->start(fail_ok => 1), 0, 'L6 start refused on corrupt registry header');
my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_off);
like($log, qr/WAL state registry .* failed validation/,
	'L6 FATAL names the registry validation failure (53RA2)');
{
	open my $fh, '+<:raw', $regfile or die;
	syswrite($fh, $hdr_orig, 512) == 512 or die;
	close $fh;
}
$node->start;
is(dumpkey($node, 'registry_ready'), 't', 'L6 restored header validates again');

# ============================================================
# L7: publish failure (read-only registry) -> FATAL 53RA2.
# ============================================================
$node->stop;
chmod(0444, $regfile) or die "chmod: $!";
$log_off = -s $node->logfile;
is($node->start(fail_ok => 1), 0, 'L7 start refused when ACTIVE publish cannot write');
$log = PostgreSQL::Test::Utils::slurp_file($node->logfile, $log_off);
like($log, qr/could not publish ACTIVE to the WAL state registry/,
	'L7 FATAL names the ACTIVE publish failure (53RA2)');
chmod(0644, $regfile) or die "chmod: $!";
$node->start;

# ============================================================
# L8: corrupt neighbour slots never block this node.
# ============================================================
$node->stop;
patch_byte($regfile, 512 + (9 - 1) * 512 + 4);    # slot 9 garbage
$node->start;
is(dumpkey($node, 'registry_slot_state'), 'active',
	'L8 own slot unaffected by a corrupt neighbour slot');

# ============================================================
# L9: no wal_threads_dir -> no registry.
# ============================================================
my $flat = PgracClusterNode->new('wal_state_flat');
$flat->init;
$flat->append_conf('postgresql.conf',
	"cluster.enabled = on\ncluster.node_id = 5\ncluster.allow_single_node = on\n");
$flat->start;
is(dumpkey($flat, 'registry_ready'), 'f', 'L9 flat layout has no registry');
is(dumpkey($flat, 'registry_slot_state'), '-', 'L9 slot state placeholder');
$flat->stop;

# ============================================================
# L9b: recovery-path failure does not publish ACTIVE.
# ============================================================
$node->stop;
my $stopped_before = read_slot_raw($regfile, 4);
is($stopped_before->{state}, 2, 'L9b precondition: clean STOPPED on disk');
make_path("$wroot/thread_9");
my $pg_wal = $node->data_dir . '/pg_wal';
unlink($pg_wal) or die "unlink: $!";
symlink("$wroot/thread_9", $pg_wal) or die "symlink: $!";
is($node->start(fail_ok => 1), 0, 'L9b mis-linked pg_wal still refused (4.1)');
{
	my $slot = read_slot_raw($regfile, 4);
	is($slot->{state}, 2,
		'L9b failed startup never published ACTIVE (slot keeps STOPPED)');
}
unlink($pg_wal) or die;
symlink("$wroot/thread_4", $pg_wal) or die;
$node->start;

# ============================================================
# L10: dump keys + wait events self-enumeration.
# ============================================================
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_cluster_state WHERE category = 'wal_thread'}),
	'10', 'L10 wal_thread category has exactly 10 keys (spec-4.2 +5)');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_stat_cluster_wait_events
		  WHERE name IN ('ClusterWalStateRead', 'ClusterWalStateWrite')}),
	'2', 'L10 registry I/O wait events registered');

$node->stop;

done_testing();
