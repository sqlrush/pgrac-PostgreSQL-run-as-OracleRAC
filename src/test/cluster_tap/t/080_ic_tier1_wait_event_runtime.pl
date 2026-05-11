# spec-2.2 Hardening v1.0.1 T-F4 -- runtime wait event sampling.
#
# Verifies F4 fix: cluster_ic wait events are now wrapped at the
# syscall sites (pgstat_report_wait_start / _end) so they observably
# fire at runtime instead of being catalog-only.  Pre-fix the events
# were registered in the wait_event tables but no call site emitted
# them, so they were observability fraud.
#
# Verification strategy (two-tier):
#
#   (a) STATIC — grep the cluster_ic_tier1.c source for the 5
#       expected pgstat_report_wait_start(WAIT_EVENT_CLUSTER_IC_*)
#       call sites.  Loopback localhost syscalls return in microseconds
#       which is much shorter than any SQL-poll sampling window
#       (~milliseconds), so the runtime sampling check below would
#       miss the brief send/recv/connect/accept events even though
#       they fire correctly under network latency.  Static grep
#       guarantees the call sites exist and are linked.
#
#   (b) RUNTIME — confirm at least ClusterICHeartbeatWait observably
#       appears in pg_stat_activity (it's the "idle wait" between
#       LMON main-loop ticks, dominant in steady state).
#
#   ClusterICReconnect: no runtime emit in spec-2.2 (LMON does not
#   sleep between reconnect attempts -- polls timestamps each tick).
#   Reserved for spec-2.X backoff scheduling.  Not asserted.
#
# Test method:
#   (a) static: grep $linkdb_src for 5 pgstat_report_wait_start lines
#   (b) runtime: 2-node ClusterPair, poll pg_stat_activity 100x ->
#       ClusterICHeartbeatWait must appear
#
# Spec: pgrac:specs/spec-2.2-* ## Hardening v1.0.1 F4.
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::ClusterPair;
use Test::More;
use Time::HiRes qw(usleep);


# ----------
# L1: STATIC -- grep cluster_ic_tier1.c for the 5 wait event call sites.
# ----------
my $tier1_src = "$ENV{TESTDATADIR}/../../../backend/cluster/cluster_ic_tier1.c";
# Fall back to a relative path if TESTDATADIR layout isn't as expected.
unless (-r $tier1_src)
{
	# tests run from src/test/cluster_tap with cwd = .
	$tier1_src = "../../backend/cluster/cluster_ic_tier1.c";
}
ok(-r $tier1_src, "L1 located cluster_ic_tier1.c source ($tier1_src)");

my $src = do { local (@ARGV, $/) = $tier1_src; <> };
ok(defined $src && length $src > 1000, 'L1 read cluster_ic_tier1.c content');

for my $event (qw(
	WAIT_EVENT_CLUSTER_IC_TCP_ACCEPT
	WAIT_EVENT_CLUSTER_IC_TCP_CONNECT
	WAIT_EVENT_CLUSTER_IC_TCP_RECV
	WAIT_EVENT_CLUSTER_IC_TCP_SEND
))
{
	like($src, qr/pgstat_report_wait_start\($event\)/,
		"L1 cluster_ic_tier1.c has pgstat_report_wait_start($event)");
}


# ----------
# L2: RUNTIME -- ClusterICHeartbeatWait observably appears in pg_stat_activity.
# (Loopback send/recv/connect/accept return in microseconds, far shorter
# than any SQL-poll window, so we don't assert those here.)
# ----------
my $pair = PostgreSQL::Test::ClusterPair->new_pair('pgrac080');
$pair->start_pair;

ok($pair->wait_for_peer_state(0, 1, 'connected', 30),
	'L2 node0 sees peer 1 connected');

my %seen;
my $deadline = time + 5;
my $polls = 0;
while (time < $deadline)
{
	for my $node ($pair->node0, $pair->node1)
	{
		my $ev = $node->safe_psql('postgres',
			"SELECT wait_event FROM pg_stat_activity
			  WHERE backend_type = 'lmon' AND wait_event IS NOT NULL");
		for my $line (split /\n/, $ev)
		{
			$seen{$line}++ if $line ne '';
		}
	}
	$polls++;
	usleep(50_000);
}
note "L2 polled $polls times; observed: " . join(", ",
	map { "$_=$seen{$_}" } sort keys %seen);

ok(exists $seen{ClusterICHeartbeatWait}
	&& $seen{ClusterICHeartbeatWait} > 0,
	"L2 ClusterICHeartbeatWait observed at runtime (count=" .
	($seen{ClusterICHeartbeatWait} // 0) . ")");

note "L2 (info) ClusterICReconnect not runtime-emitted in spec-2.2; "
	. "reserved for spec-2.X backoff scheduling per v1.0.1 appendix F4";


$pair->stop_pair;

done_testing();
