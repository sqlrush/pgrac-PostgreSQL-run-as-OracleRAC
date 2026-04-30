/*-------------------------------------------------------------------------
 *
 * cluster_pgstat.h
 *	  pgrac cluster performance-stats framework (Stage 0.28).
 *
 *	  Provides a per-process atomic counter registry that backs the
 *	  pg_stat_cluster_counters view.  Stage 1+ subsystems append their
 *	  own counters to the central array in cluster_pgstat.c and
 *	  increment them via the helpers declared here; the framework
 *	  surfaces every counter through a single SRF without per-counter
 *	  bookkeeping.
 *
 *	  Design mirrors spec-0.27 (cluster_inject) -- compile-time static
 *	  registry, lazy initialisation, single-direction module dependency.
 *	  Cross-process / shmem aggregation is deferred to Stage 2+ when
 *	  the cluster shmem coordination protocol lands.
 *
 *	  See specs/spec-0.28-perfmon-framework.md for stage-0 scope and
 *	  docs/performance-views-design.md (43-view SSOT) for the Stage 1+
 *	  counter / view roadmap.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_pgstat.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  The internal helpers (cluster_pgstat_lookup / inc / set / read)
 *	  are available only when configured with --enable-cluster
 *	  (USE_PGRAC_CLUSTER defined).  The SRF entry point
 *	  cluster_get_pgstat_counters is always linked because pg_proc.dat
 *	  references it unconditionally; its body raises ERRCODE_FEATURE_
 *	  NOT_SUPPORTED in disable-cluster builds.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_PGSTAT_H
#define CLUSTER_PGSTAT_H

#include "fmgr.h"
#include "port/atomics.h"


/* ----------
 * ClusterPgstatCounter -- one named atomic counter.
 *
 *	The `name` is a compile-time string literal owned by the registry
 *	in cluster_pgstat.c; callers must not mutate it.  `value` is a
 *	pg_atomic_uint64 to allow lock-free increment from any backend.
 * ---------- */
typedef struct ClusterPgstatCounter {
	const char *name;
	pg_atomic_uint64 value;
} ClusterPgstatCounter;


#ifdef USE_PGRAC_CLUSTER

/* ----------
 * Public helpers.  All four are no-ops if the registry has not been
 * lazy-initialised yet (the helpers themselves trigger init).  In
 * disable-cluster builds these symbols do not exist (spec-0.3 contract).
 * ---------- */
extern ClusterPgstatCounter *cluster_pgstat_lookup(const char *name);
extern void cluster_pgstat_inc(ClusterPgstatCounter *c);
extern void cluster_pgstat_set(ClusterPgstatCounter *c, uint64 value);
extern uint64 cluster_pgstat_read(const ClusterPgstatCounter *c);

#endif /* USE_PGRAC_CLUSTER */


/* ----------
 * SRF entry points.  Always linked (pg_proc.dat references these
 * unconditionally); body is #ifdef USE_PGRAC_CLUSTER guarded and
 * raises ERRCODE_FEATURE_NOT_SUPPORTED on disable-cluster builds.
 *
 *	cluster_get_pgstat_counters()
 *	    RETURNS SETOF (name text, value int8)
 *	    Returns one row per registered counter.  Counters that mirror
 *	    external state (e.g. cluster.inject.armed_count) are sync'd
 *	    at SRF entry by reading the source variable.  Powers the
 *	    pg_stat_cluster_counters view.
 * ---------- */
extern Datum cluster_get_pgstat_counters(PG_FUNCTION_ARGS);

#endif /* CLUSTER_PGSTAT_H */
