
# spec-2.36 D15 (HC121) -- ClusterTriple: 3-instance harness for CF 3-way
# protocol TAP tests.
#
# Encapsulates the boilerplate that 115_gcs_block_3way_3node.pl (and future
# Stage 2.X 3-way / 3-node tests) need to spin up three cooperating pgrac
# instances with mutual interconnect:
#   - allocate 6 random free ports (3 PG + 3 IC)
#   - init all three nodes
#   - append cluster.* GUCs (cluster.enabled = on, tier = tier1, node_id 0/1/2)
#   - write mutually-trusting pgrac.conf to all three data dirs
#   - start_triple / stop_triple helpers
#
# Mirrors PostgreSQL::Test::ClusterPair from spec-2.2 D15;  same structure
# with an extra node slot.  Used to verify:
#   - 3-node S holders bitmap broadcast invalidate ack collection
#   - X transfer A→B + concurrent N→S on C exercising HC117 S barrier
#   - non-strict FIFO multi-pending X requests (Q-D5 trade-off)
#
# Spec authority: pgrac:specs/spec-2.36-* §1.2 D15 + §4.3 TAP 115.
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
#-------------------------------------------------------------------------

package PostgreSQL::Test::ClusterTriple;

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;


#-----------------------------------------------------------------------
# new_triple($class, $cluster_name, %opts)
#
#	Allocate three PG instances that share a pgrac cluster name.
#	Optional %opts:
#	  extra_conf  : arrayref of extra GUC lines appended to ALL nodes'
#	                postgresql.conf
#-----------------------------------------------------------------------
sub new_triple
{
	my ($class, $cluster_name, %opts) = @_;

	# Allocate 6 distinct free ports.
	my @pg_ports = (
		PostgreSQL::Test::Cluster::get_free_port(),
		PostgreSQL::Test::Cluster::get_free_port(),
		PostgreSQL::Test::Cluster::get_free_port(),
	);
	my @ic_ports = (
		PostgreSQL::Test::Cluster::get_free_port(),
		PostgreSQL::Test::Cluster::get_free_port(),
		PostgreSQL::Test::Cluster::get_free_port(),
	);

	my @nodes;
	for my $i (0 .. 2)
	{
		push @nodes,
		  PostgreSQL::Test::Cluster->new(
			"${cluster_name}_node$i", port => $pg_ports[$i]);
	}

	for my $node (@nodes)
	{
		$node->init;

		# Enable cluster + tier1, same baseline as ClusterPair (spec-2.2).
		$node->append_conf('postgresql.conf', "cluster.enabled = on\n");
		$node->append_conf('postgresql.conf',
			"cluster.interconnect_tier = tier1\n");
		$node->append_conf('postgresql.conf',
			"cluster.allow_single_node = on\n");

		# Keep shared_buffers small so 3 postmasters fit in CI runners.
		$node->append_conf('postgresql.conf', "shared_buffers = 16MB\n");

		if ($opts{extra_conf})
		{
			for my $line (@{ $opts{extra_conf} })
			{
				$node->append_conf('postgresql.conf', "$line\n");
			}
		}
	}

	# Per-node identity.
	for my $i (0 .. 2)
	{
		$nodes[$i]->append_conf('postgresql.conf', "cluster.node_id = $i\n");
	}

	# Build the shared pgrac.conf body declaring all three peers.
	my $peers_block = "";
	for my $i (0 .. 2)
	{
		$peers_block .=
		  "[node.$i]\ninterconnect_addr = 127.0.0.1:$ic_ports[$i]\n\n";
	}

	my $pgrac_conf_body = <<EOC;
[cluster]
name = $cluster_name

$peers_block
EOC

	for my $node (@nodes)
	{
		PostgreSQL::Test::Utils::append_to_file(
			$node->data_dir . '/pgrac.conf', $pgrac_conf_body);
	}

	return bless {
		nodes       => \@nodes,
		cluster_name => $cluster_name,
		pg_ports    => \@pg_ports,
		ic_ports    => \@ic_ports,
	}, $class;
}


sub start_triple
{
	my ($self, %opts) = @_;
	for my $node (@{ $self->{nodes} })
	{
		$node->start(%opts);
	}

	# Diagnostic note (mirrors ClusterPair pattern).
	my $name = $self->{cluster_name} // '(unknown)';
	my $msg = "ClusterTriple started: cluster_name='$name'";
	for my $i (0 .. 2)
	{
		my $pg = $self->{nodes}[$i]->port;
		my $ic = $self->{ic_ports}[$i] // -1;
		$msg .= " node$i=pg:$pg/ic:$ic";
	}
	Test::More::note($msg);
	return;
}


sub stop_triple
{
	my ($self) = @_;
	for my $node (@{ $self->{nodes} })
	{
		$node->stop if $node;
	}
	return;
}


sub node      { return $_[0]->{nodes}[ $_[1] ]; }
sub node0     { return $_[0]->{nodes}[0]; }
sub node1     { return $_[0]->{nodes}[1]; }
sub node2     { return $_[0]->{nodes}[2]; }
sub ic_port   { return $_[0]->{ic_ports}[ $_[1] ]; }
sub pg_port   { return $_[0]->{pg_ports}[ $_[1] ]; }
sub cluster_name { return $_[0]->{cluster_name}; }


1;
