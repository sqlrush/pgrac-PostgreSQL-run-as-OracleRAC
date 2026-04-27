/*-------------------------------------------------------------------------
 *
 * cluster_guc.h
 *	  pgrac cluster GUC registration entry point and exported variables.
 *
 *	  This header is the single source of truth for cluster GUC C
 *	  declarations.  Stage 0.13 introduces the registration mechanism
 *	  and activates the first cluster GUC, cluster_node_id (originally
 *	  a placeholder global in cluster_elog.c, see spec-0.9).  Future
 *	  cluster GUCs land here at the same time their owning subsystem
 *	  spec is implemented; see docs/cluster-guc-design.md §3.1 for the
 *	  Single Source of Truth registration roster.
 *
 *	  Responsibilities of this header:
 *
 *	  - Declare extern storage for every registered cluster GUC C
 *	    variable, so the rest of the cluster code can read its current
 *	    value without going through the SQL GUC layer.
 *	  - Declare cluster_init_guc(), the registration entry point that
 *	    PostmasterMain calls (under #ifdef USE_PGRAC_CLUSTER) right after
 *	    PG's built-in GUCs are loaded.
 *
 *	  Stage 0.13 does NOT register the ~24 cluster GUCs listed in the
 *	  background-process / error-codes design docs.  Each remaining
 *	  GUC is registered together with the spec that introduces its
 *	  first reader (CLAUDE.md rule 8).  That policy is documented in
 *	  docs/cluster-guc-design.md §1 and §7.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_guc.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  The header is intentionally PG-free at the declaration level so
 *	  cluster_unit standalone tests can include it; the implementation
 *	  in cluster_guc.c does include "utils/guc.h" because
 *	  DefineCustomIntVariable() lives there.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_GUC_H
#define CLUSTER_GUC_H


/*
 * cluster_init_guc -- register all cluster GUC variables.
 *
 *	Called from PostmasterMain after InitializeGUCOptions() has loaded
 *	PG's built-in GUCs.  Subsequent postgresql.conf parsing applies
 *	user-supplied values, so registration must happen first.
 *
 *	Idempotent within a process: PG's GUC layer rejects duplicate
 *	registrations with the same name, so calling this twice would
 *	abort startup.  PostmasterMain only calls it once.
 *
 *	Implementation lives in src/backend/cluster/cluster_guc.c.
 */
extern void cluster_init_guc(void);


/* ============================================================
 * Cluster GUC variables (storage owned by cluster_guc.c).
 *
 *	Declared here so that callers (e.g. CLUSTER_LOG in cluster_elog.h)
 *	can read the current value without locking.  Each value is updated
 *	by PG's GUC machinery on startup or reload.
 * ============================================================ */

/*
 * cluster_node_id -- numeric identifier of this node in the cluster.
 *
 *	-1  = unconfigured (running outside a cluster)
 *	0..127 = node id (range covers Stage 1+ planned 16-128 node clusters,
 *	         see CLAUDE.md AD-012 example 10).
 *
 *	context: PGC_POSTMASTER (requires server restart to change).
 */
extern int cluster_node_id;


#endif /* CLUSTER_GUC_H */
