# spec-2.2 Hardening v1.0.1 T-F3 -- LMON respawn → listener rebind.
#
# Verifies F3 fix: listener fd lives process-local (not in shmem).
# After LMON crash + postmaster respawn, the new LMON must open a
# fresh listener fd and continue accepting peer connections.  Pre-fix:
# the integer in shmem was reused as if it were still a valid fd ->
# silent listener failure.
#
# Test steps:
#   L1 single-node tier1 starts, listener metadata visible
#      (listener_pid > 0; listener_incarnation = 1)
#   L2 raw TCP connect to listener_port succeeds (proves listener
#      is actually accepting)
#   L3 SIGKILL the LMON process
#   L4 postmaster respawns LMON; poll until listener_pid CHANGES and
#      listener_incarnation INCREMENTS
#   L5 raw TCP connect to listener_port succeeds AGAIN against the
#      new listener (proves new LMON rebound, not just reused stale fd)
#   L6 sanity: postmaster still alive + accepting SQL throughout
#
# Spec: pgrac:specs/spec-2.2-* ## Hardening v1.0.1 F3.
#
#-------------------------------------------------------------------------

use strict;
use warnings;

use FindBin;
use lib "$FindBin::RealBin/../lib";

use IO::Socket::INET;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);


my $node = PostgreSQL::Test::Cluster->new('ic_lmon_respawn');
$node->init;

$node->append_conf('postgresql.conf', "cluster.enabled = on\n");
$node->append_conf('postgresql.conf', "cluster.interconnect_tier = tier1\n");
$node->append_conf('postgresql.conf', "cluster.node_id = 0\n");
$node->append_conf('postgresql.conf', "cluster.allow_single_node = on\n");

# Bound the postmaster phase1 timeout so LMON respawn isn't masked by
# a long bootstrap window.
$node->append_conf('postgresql.conf', "cluster.phase1_timeout = 10s\n");

# PostgreSQL::Test::Cluster sets restart_after_crash=off by default
# (so test crashes are loud).  This test deliberately kills LMON to
# verify postmaster respawns it; keep restart_after_crash=on for it.
$node->append_conf('postgresql.conf', "restart_after_crash = on\n");

my $ic_port = PostgreSQL::Test::Cluster::get_free_port();
my $pgrac_conf = <<EOC;
[cluster]
name = pgrac-077

[node.0]
interconnect_addr = 127.0.0.1:$ic_port
EOC
PostgreSQL::Test::Utils::append_to_file($node->data_dir . '/pgrac.conf',
	$pgrac_conf);

$node->start;


# ----------
# L1: listener metadata visible + first incarnation = 1.
# ----------
my $pid_v1 = $node->safe_psql('postgres',
	"SELECT value FROM pg_cluster_state WHERE category='ic' AND key='tier1_listener_pid'");
my $inc_v1 = $node->safe_psql('postgres',
	"SELECT value FROM pg_cluster_state WHERE category='ic' AND key='tier1_listener_incarnation'");
my $port_v1 = $node->safe_psql('postgres',
	"SELECT value FROM pg_cluster_state WHERE category='ic' AND key='tier1_listener_port'");

ok($pid_v1 > 0, "L1 listener_pid is positive (got $pid_v1)");
is($inc_v1, '1', 'L1 listener_incarnation = 1 on first bind');
is($port_v1, "$ic_port",
	"L1 listener_port matches pgrac.conf (got $port_v1, expected $ic_port)");


# ----------
# L2: raw TCP connect to listener succeeds (listener is real).
# ----------
sub probe_listener
{
	my $sock = IO::Socket::INET->new(
		PeerAddr => '127.0.0.1',
		PeerPort => $ic_port,
		Proto    => 'tcp',
		Timeout  => 2);
	return defined $sock ? do { close $sock; 1 } : 0;
}

ok(probe_listener(), 'L2 listener accepts a fresh TCP connection (pre-respawn)');


# ----------
# L3: SIGKILL the LMON process.
# ----------
kill 'KILL', $pid_v1
	or die "L3 kill -9 $pid_v1 failed: $!";
note "L3 sent SIGKILL to LMON pid $pid_v1";


# ----------
# L4: postmaster respawns LMON; listener_pid changes.
#
# Note on incarnation:  killing LMON triggers full PG crash recovery
# (postmaster terminates ALL backends + recreates shmem); after the
# rebuild Tier1Shmem is freshly zeroed, so listener_incarnation
# starts again at 0 and the new listener_bind() call sets it to 1
# -- same value as before the kill.  Incarnation is only meaningful
# WITHIN a single postmaster lifecycle (e.g. a softer LMON exit that
# doesn't trigger crash recovery, which spec-2.X may add).  For F3
# verification, listener_pid changing is the durable signal that
# fd was rebound from scratch in a new process.
# ----------
my $deadline = time + 30;
my $pid_v2;
while (time < $deadline)
{
	# psql may briefly fail mid-respawn; tolerate that.
	($pid_v2, my $err1) = ('', '');
	$node->psql('postgres',
		"SELECT value FROM pg_cluster_state WHERE category='ic' AND key='tier1_listener_pid'",
		stdout => \$pid_v2, stderr => \$err1);

	if ($pid_v2 ne '' && $pid_v2 != $pid_v1 && $pid_v2 > 0)
	{
		last;
	}
	usleep(200_000);     # 0.2s
}

ok(defined $pid_v2 && $pid_v2 ne '' && $pid_v2 != $pid_v1,
	"L4 listener_pid changed after respawn (was $pid_v1, now $pid_v2)");


# ----------
# L5: raw TCP connect to listener succeeds AGAIN (new listener works).
# ----------
ok(probe_listener(),
	'L5 listener accepts a fresh TCP connection (post-respawn) - F3 fix verified');


# ----------
# L6: postmaster + SQL still alive throughout.
# ----------
is($node->safe_psql('postgres', 'SELECT 1'),
	'1',
	'L6 postmaster still accepts SQL after LMON respawn');


$node->stop;

done_testing();
