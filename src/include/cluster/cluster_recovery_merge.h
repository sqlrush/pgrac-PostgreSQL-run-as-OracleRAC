/*-------------------------------------------------------------------------
 *
 * cluster_recovery_merge.h
 *	  pgrac k-way SCN merge replay engine (spec-4.5).
 *
 *	  Cold-crash merged recovery (engage only when no foreign node is
 *	  ALIVE) reads the WAL streams of the merge set (crash candidates
 *	  plus own) and replays their records in global SCN order.  This
 *	  header holds the PURE, unit-testable core:
 *
 *	    - cluster_recovery_merge_cmp(): the (xl_scn -> lsn -> node)
 *	      three-level ordering, delegated to scn_recovery_cmp so the
 *	      SCN-cmp gate stays satisfied.
 *	    - a small binary min-heap over per-stream cursors keyed by that
 *	      ordering (k <= 16 streams; insert/pop are O(log k)).
 *	    - cluster_recovery_record_class(): the §3.3e G/S/L/U apply
 *	      matrix as a pure rmid + has-block-ref + is-shared-block
 *	      decision (the caller supplies is_shared because the smgr
 *	      routing predicate is backend-only).
 *
 *	  The backend engine (cluster_recovery_merge.c) wraps real
 *	  XLogReaders around these and drives ApplyWalRecord; the §3.3b
 *	  freshness contract and §3.3c candidate self-recovery authority
 *	  live there.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_recovery_merge.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-4.5-kway-scn-merge-replay.md FROZEN v1.0
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_RECOVERY_MERGE_H
#define CLUSTER_RECOVERY_MERGE_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "cluster/cluster_scn.h"
#include "cluster/cluster_wal_state.h"

/*
 * §3.3e apply class.  Per record, decided from the rmid white-table +
 * whether it carries a block reference + (for block records) whether
 * the first block routes to shared storage.
 */
typedef enum ClusterRecoveryRecordClass {
	CLUSTER_RECMERGE_GLOBAL = 0,	 /* G: XACT/CLOG/MULTIXACT/STANDBY/COMMIT_TS --
								  * per-node replicated logical state; applied on
								  * BOTH the foreign-merge and self-recovery
								  * paths (A needs the peer's commit bits) */
	CLUSTER_RECMERGE_SHARED,		 /* S: block ref routed to shared storage --
								  * the merge body; A applies, B skips <=
								  * merge_recovered_lsn */
	CLUSTER_RECMERGE_LOCAL,			 /* L: node-local (catalog pages, RELMAP, XLOG
								  * housekeeping) -- A skips a foreign stream's
								  * L record (it would clobber A's own files),
								  * B applies its own */
	CLUSTER_RECMERGE_UNCLASSIFIABLE, /* U: cannot be safely routed (DBASE/
									  * TBLSPC, or a mixed S/L block set) --
									  * fail-closed 53RA3 on the foreign path */
} ClusterRecoveryRecordClass;

/*
 * cluster_recovery_record_class -- the §3.3e matrix.
 *
 *	has_block_ref: the record registered at least one buffer.
 *	first_block_is_shared: for a block record, whether the FIRST block's
 *	    RelFileLocator routes to shared storage (caller's smgr predicate;
 *	    ignored when has_block_ref is false).
 *	all_blocks_same_routing: false when a single record mixes shared and
 *	    local blocks -> U (fail-closed; expected never to occur).
 */
static inline ClusterRecoveryRecordClass
cluster_recovery_record_class(RmgrId rmid, bool has_block_ref, bool first_block_is_shared,
							  bool all_blocks_same_routing)
{
	/* G: global logical state carried by WAL, no block reference. */
	if (!has_block_ref) {
		switch (rmid) {
		case RM_XACT_ID:
		case RM_CLOG_ID:
		case RM_MULTIXACT_ID:
		case RM_STANDBY_ID:
		case RM_COMMIT_TS_ID:
			return CLUSTER_RECMERGE_GLOBAL;
		case RM_XLOG_ID:
		case RM_RELMAP_ID:
			/* XLOG housekeeping (checkpoint/fpw/...) + relmap are
			 * node-local on a foreign stream. */
			return CLUSTER_RECMERGE_LOCAL;
		case RM_DBASE_ID:
		case RM_TBLSPC_ID:
			/* createdb / tablespace ops touch local dirs we cannot
			 * safely re-drive from a foreign stream. */
			return CLUSTER_RECMERGE_UNCLASSIFIABLE;
		default:
			/* An unlisted no-block-ref record is not safe to assume
			 * either way. */
			return CLUSTER_RECMERGE_UNCLASSIFIABLE;
		}
	}

	/* Block records: a mixed routing set is never expected -> U. */
	if (!all_blocks_same_routing)
		return CLUSTER_RECMERGE_UNCLASSIFIABLE;

	return first_block_is_shared ? CLUSTER_RECMERGE_SHARED : CLUSTER_RECMERGE_LOCAL;
}

/* ---- k-way merge min-heap (pure) ---- */

#define CLUSTER_RECMERGE_MAX_STREAMS 16

typedef struct ClusterRecmergeKey {
	uint64 scn;
	XLogRecPtr lsn;
	int32 node; /* tid - 1 */
} ClusterRecmergeKey;

typedef struct ClusterRecmergeHeapEntry {
	ClusterRecmergeKey key;
	int stream; /* index into the engine's per-stream cursor array */
} ClusterRecmergeHeapEntry;

typedef struct ClusterRecmergeHeap {
	ClusterRecmergeHeapEntry e[CLUSTER_RECMERGE_MAX_STREAMS];
	int n;
} ClusterRecmergeHeap;

/* Strict weak ordering: a < b ?  (scn -> lsn -> node, via the gated cmp). */
static inline int
cluster_recovery_merge_cmp(const ClusterRecmergeKey *a, const ClusterRecmergeKey *b)
{
	return scn_recovery_cmp((SCN)a->scn, a->lsn, (NodeId)a->node, (SCN)b->scn, b->lsn,
							(NodeId)b->node);
}

static inline void
cluster_recmerge_heap_init(ClusterRecmergeHeap *h)
{
	h->n = 0;
}

static inline void
cluster_recmerge_heap_push(ClusterRecmergeHeap *h, ClusterRecmergeKey key, int stream)
{
	int i;

	Assert(h->n < CLUSTER_RECMERGE_MAX_STREAMS);
	i = h->n++;
	h->e[i].key = key;
	h->e[i].stream = stream;
	while (i > 0) {
		int parent = (i - 1) / 2;

		if (cluster_recovery_merge_cmp(&h->e[i].key, &h->e[parent].key) >= 0)
			break;
		{
			ClusterRecmergeHeapEntry t = h->e[i];

			h->e[i] = h->e[parent];
			h->e[parent] = t;
		}
		i = parent;
	}
}

/* Pop the minimum (NULL when empty); returns the stream index + key. */
static inline bool
cluster_recmerge_heap_pop(ClusterRecmergeHeap *h, ClusterRecmergeHeapEntry *out)
{
	int i = 0;

	if (h->n == 0)
		return false;
	*out = h->e[0];
	h->e[0] = h->e[--h->n];
	while (true) {
		int l = 2 * i + 1;
		int r = 2 * i + 2;
		int sm = i;

		if (l < h->n && cluster_recovery_merge_cmp(&h->e[l].key, &h->e[sm].key) < 0)
			sm = l;
		if (r < h->n && cluster_recovery_merge_cmp(&h->e[r].key, &h->e[sm].key) < 0)
			sm = r;
		if (sm == i)
			break;
		{
			ClusterRecmergeHeapEntry t = h->e[i];

			h->e[i] = h->e[sm];
			h->e[sm] = t;
		}
		i = sm;
	}
	return true;
}

#ifndef FRONTEND

/* Backend engine (cluster_recovery_merge.c). */
typedef struct ClusterRecoveryMergeState ClusterRecoveryMergeState;

extern ClusterRecoveryMergeState *cluster_recovery_merge_begin(const uint64 merge_bitmap[2],
															   const XLogRecPtr *start_lsn,
															   TimeLineID tli);
extern struct XLogReaderState *cluster_recovery_merge_next(ClusterRecoveryMergeState *st,
														   uint16 *thread_out, char **errmsg_out);
extern void cluster_recovery_merge_end(ClusterRecoveryMergeState *st);

#endif /* !FRONTEND */

#endif /* CLUSTER_RECOVERY_MERGE_H */
