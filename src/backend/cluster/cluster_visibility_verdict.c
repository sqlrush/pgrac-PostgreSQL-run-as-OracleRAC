/*-------------------------------------------------------------------------
 *
 * cluster_visibility_verdict.c
 *	  pgrac spec-3.14 §2.2 OBS truth tables as pure verdict functions.
 *
 *	  Split out from cluster_visibility_resolve.c so the truth-table
 *	  policy (status -> verdict) carries ZERO PG-backend dependency and
 *	  can be unit-tested by full enumeration on its own.  The five
 *	  HeapTupleSatisfies* forks (Update/Dirty/Self/Toast) and the unit
 *	  test are the only callers; this is the single source of truth for
 *	  the OBS-2~5 tables (L212).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_visibility_verdict.c
 *
 * NOTES
 *	  This is a pgrac-original file.  Compiled only in --enable-cluster
 *	  builds.
 *	  Spec: spec-3.14-remaining-visibility-paths.md (FROZEN v0.2) §2.2.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_PGRAC_CLUSTER

#include "cluster/cluster_tt_status.h"
#include "cluster/cluster_visibility_resolve.h"

/* ============================================================
 *	spec-3.14 §2.2 OBS truth tables (pure verdict functions).
 *
 *	Input is a terminal-or-in-progress ClusterTTStatus (the resolver
 *	already followed SUBCOMMITTED to its parent; a residual SUBCOMMITTED
 *	is treated as in-progress, defensively).  COMMITTED and CLEANED_OUT
 *	are the two "committed" states and behave identically here.
 * ============================================================ */

ClusterVisVerdict
cluster_vis_self_verdict(ClusterTTStatus status)
{
	switch (status) {
	case CLUSTER_TT_STATUS_COMMITTED:
	case CLUSTER_TT_STATUS_CLEANED_OUT:
		return CVV_VISIBLE;
	case CLUSTER_TT_STATUS_IN_PROGRESS:
	case CLUSTER_TT_STATUS_SUBCOMMITTED:
	case CLUSTER_TT_STATUS_ABORTED:
		return CVV_INVISIBLE;
	case CLUSTER_TT_STATUS_UNKNOWN:
	default:
		return CVV_FAILCLOSED_UNKNOWN;
	}
}

ClusterVisVerdict
cluster_vis_toast_verdict(ClusterTTStatus status)
{
	/* OBS-5: toast is read only after the main row passed visibility, so
	 * PG-native is permissive -- only an ABORTED writer hides the chunk. */
	switch (status) {
	case CLUSTER_TT_STATUS_ABORTED:
		return CVV_INVISIBLE;
	case CLUSTER_TT_STATUS_UNKNOWN:
		return CVV_FAILCLOSED_UNKNOWN; /* a torn toast chain must be heard */
	case CLUSTER_TT_STATUS_COMMITTED:
	case CLUSTER_TT_STATUS_CLEANED_OUT:
	case CLUSTER_TT_STATUS_IN_PROGRESS:
	case CLUSTER_TT_STATUS_SUBCOMMITTED:
	default:
		return CVV_VISIBLE;
	}
}

ClusterVisVerdict
cluster_vis_update_xmin_verdict(ClusterTTStatus status)
{
	switch (status) {
	case CLUSTER_TT_STATUS_COMMITTED:
	case CLUSTER_TT_STATUS_CLEANED_OUT:
		return CVV_VISIBLE; /* proceed to xmax judgement */
	case CLUSTER_TT_STATUS_IN_PROGRESS:
	case CLUSTER_TT_STATUS_SUBCOMMITTED:
	case CLUSTER_TT_STATUS_ABORTED:
		return CVV_INVISIBLE;
	case CLUSTER_TT_STATUS_UNKNOWN:
	default:
		return CVV_FAILCLOSED_UNKNOWN;
	}
}

ClusterVisVerdict
cluster_vis_update_xmax_verdict(ClusterTTStatus status, bool is_delete)
{
	switch (status) {
	case CLUSTER_TT_STATUS_ABORTED:
		return CVV_VISIBLE; /* writer aborted -> tuple still ok (TM_Ok) */
	case CLUSTER_TT_STATUS_COMMITTED:
	case CLUSTER_TT_STATUS_CLEANED_OUT:
		return is_delete ? CVV_GONE_DELETED : CVV_GONE_UPDATED;
	case CLUSTER_TT_STATUS_IN_PROGRESS:
	case CLUSTER_TT_STATUS_SUBCOMMITTED:
		/* spec-3.14 C-V4: the Satisfies layer only flags this; the
		 * caller-side wait bridge (D2b) fail-closes to 53R9H. */
		return CVV_BEING_MODIFIED;
	case CLUSTER_TT_STATUS_UNKNOWN:
	default:
		return CVV_FAILCLOSED_UNKNOWN;
	}
}

ClusterVisVerdict
cluster_vis_dirty_verdict(ClusterTTStatus status, bool is_xmax, bool is_delete)
{
	switch (status) {
	case CLUSTER_TT_STATUS_IN_PROGRESS:
	case CLUSTER_TT_STATUS_SUBCOMMITTED:
		/* OBS-3: Dirty has no wait_policy layer -> cross-node pending is a
		 * conflict the caller cannot wait on locally (53R9H). */
		return CVV_FAILCLOSED_CONFLICT;
	case CLUSTER_TT_STATUS_ABORTED:
		return is_xmax ? CVV_VISIBLE : CVV_INVISIBLE;
	case CLUSTER_TT_STATUS_COMMITTED:
	case CLUSTER_TT_STATUS_CLEANED_OUT:
		if (!is_xmax)
			return CVV_VISIBLE; /* xmin committed -> tuple exists */
		return is_delete ? CVV_GONE_DELETED : CVV_GONE_UPDATED;
	case CLUSTER_TT_STATUS_UNKNOWN:
	default:
		return CVV_FAILCLOSED_UNKNOWN;
	}
}

#endif /* USE_PGRAC_CLUSTER */
