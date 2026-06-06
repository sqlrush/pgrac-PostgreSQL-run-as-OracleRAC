# -*- perl -*-
#
# PostgreSQL::Test::Stage3AcceptanceReport
#
#	  spec-3.17 D7:  Stage 3 MVCC acceptance JSON report emitter.
#
#	  Standardized schema (version 1):
#	    {
#	      spec: "3.17",
#	      tag:  "v0.67.0-stage3.17" | "unknown",
#	      timestamp: ISO8601,
#	      schema_version: 1,
#	      matrix: {
#	        mvcc_capabilities: [ { name, status, layer, counter_delta, ... }, ... ],
#	        workloads:         [ { name, single_node_off, single_node_on, two_node, tps, ... }, ... ],
#	        demo:              [ { name, status, note }, ... ],
#	        limitations:       [ { name, kind, forward, note }, ... ]
#	      }
#	    }
#
#	  Each test (t/226, t/227) accumulates into one report instance and
#	  emits to tmp/stage3-acceptance-<timestamp>.json.  pgrac docs/
#	  stage-3-acceptance.md + docs/perf-baseline.md manually import for
#	  trend tracking (linkdb CI does NOT cross-write the pgrac repo —
#	  mirrors spec-2.40 v0.2 F4 architectural separation).
#
#	  workloads + baseline rows are report-only (spec-3.17 §3.2):  Stage 3
#	  MVCC write decay is large + runner-noisy, so no numeric perf gate —
#	  correctness is proven by SQL assertions, not a perf floor.  Perf bar
#	  is owned by spec-3.18.
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
package PostgreSQL::Test::Stage3AcceptanceReport;

use strict;
use warnings;

use Scalar::Util qw(reftype);
use Time::HiRes qw(time);


# Hand-rolled minimal JSON emitter — avoids the JSON.pm dep on CI runners
# where Perl prereqs are minimal.  Only handles scalars / arrays / hashes
# nested to whatever depth our schema needs.
#
# Uses reftype() (not ref()) so the top-level *blessed* report object encodes
# as its underlying HASH — ref() returns the package name for a blessed ref,
# which would otherwise stringify the whole report to a garbage scalar.
sub _encode
{
	my ($v) = @_;
	if (!defined $v) {
		return 'null';
	}
	my $rt = reftype($v);
	if (defined $rt && $rt eq 'HASH') {
		my @kv;
		for my $k (sort keys %$v) {
			my $ek = $k; $ek =~ s/"/\\"/g;
			push @kv, qq{"$ek":} . _encode($v->{$k});
		}
		return '{' . join(',', @kv) . '}';
	}
	if (defined $rt && $rt eq 'ARRAY') {
		return '[' . join(',', map { _encode($_) } @$v) . ']';
	}
	# Scalar:  numeric vs string detection.  Conservative — anything not a
	# pure integer/decimal we quote.
	if ($v =~ /^-?\d+(\.\d+)?$/) {
		return $v;
	}
	my $s = $v;
	$s =~ s/\\/\\\\/g;
	$s =~ s/"/\\"/g;
	$s =~ s/\n/\\n/g;
	$s =~ s/\r/\\r/g;
	$s =~ s/\t/\\t/g;
	return qq{"$s"};
}


sub new
{
	my ($class, %opts) = @_;
	my $self = {
		spec           => $opts{spec}      // '3.17',
		tag            => $opts{tag}       // 'unknown',
		timestamp      => $opts{timestamp} // _iso_now(),
		schema_version => 1,
		matrix => {
			mvcc_capabilities => [],
			workloads         => [],
			demo              => [],
			limitations       => [],
		},
	};
	return bless $self, $class;
}


sub _iso_now
{
	my @t = localtime(time);
	return sprintf "%04d-%02d-%02dT%02d:%02d:%02d",
		$t[5]+1900, $t[4]+1, $t[3], $t[2], $t[1], $t[0];
}


# D1 (t/226):  one MVCC capability assertion (L1-L10).  status PASS / SKIP
# (best-effort when a sub-criterion is not observable — honest, mirrors
# spec-2.40 L1b);  layer hard / best_effort.
sub record_mvcc_capability
{
	my ($self, $name, %extra) = @_;
	push @{ $self->{matrix}->{mvcc_capabilities} }, {
		name   => $name,
		status => $extra{status} // 'PASS',
		layer  => $extra{layer}  // 'hard',
		%extra,
	};
}


# D2 (t/227):  one workload smoke row.  All ratio/TPS fields are report-only
# (no numeric fail);  the only hard gate is functional (pgbench succeeded,
# tps present and > 0, JSON complete, SQL correctness assertion passed),
# carried in the 'functional' field.
sub record_workload
{
	my ($self, $name, %extra) = @_;
	push @{ $self->{matrix}->{workloads} }, {
		name       => $name,
		functional => $extra{functional} // 'PASS',
		%extra,
	};
}


# D4:  one 4-node shared-storage demo row.  status PASS / SKIP (SKIP carries
# 'reason' when no external shared-storage cluster is wired — never faked,
# spec-3.17 §3.3 + V-2 honest finding).
sub record_demo
{
	my ($self, $name, %extra) = @_;
	push @{ $self->{matrix}->{demo} }, {
		name   => $name,
		status => $extra{status} // 'SKIP',
		%extra,
	};
}


# §3.4:  one honest Stage 3 limitation registration (SCN high-watermark L222 /
# cross-instance overlay 53R97 #95 / standby resolve-after / multi-node decay).
# kind = availability / correctness-forward / perf-forward;  forward = the
# spec/feature that owns the eventual fix.
sub record_limitation
{
	my ($self, $name, %extra) = @_;
	push @{ $self->{matrix}->{limitations} }, {
		name => $name,
		kind => $extra{kind} // 'forward',
		%extra,
	};
}


sub emit_json
{
	my ($self, $path) = @_;
	open my $fh, '>', $path or die "open $path: $!";
	print $fh _encode($self);
	close $fh;
	return $path;
}


# Test helper:  build default path tmp/stage3-acceptance-<timestamp>.json
sub default_path
{
	my ($self, $base) = @_;
	$base //= 'tmp';
	mkdir $base unless -d $base;
	my $ts = $self->{timestamp};
	$ts =~ s/[^0-9TZ]/_/g;
	return "$base/stage3-acceptance-$ts.json";
}


1;
