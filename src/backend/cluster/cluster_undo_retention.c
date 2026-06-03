/*-------------------------------------------------------------------------
 *
 * cluster_undo_retention.c
 *	  pgrac own-instance undo / TT-slot retention predicates (spec-3.12 D2/D3).
 *
 *	  This file holds the two PURE judgement helpers that the lazy retention
 *	  gate consults at TT-slot / undo-segment allocation time:
 *
 *	    cluster_tt_slot_recyclable()      -- per-slot allocator-status gate
 *	    cluster_undo_segment_recyclable() -- per-segment header gate
 *
 *	  Neither takes a lock, touches shmem, or does I/O: they are functions of
 *	  (status, commit_scn, horizon) only, so cluster_unit can exercise every
 *	  branch without a live postmaster (test_cluster_retention).  The horizon
 *	  itself is produced by cluster_undo_retention_horizon() in procarray.c
 *	  (it scans the ProcArray under ProcArrayLock; see C17 lock ordering).
 *
 *	  Correctness contract (CLAUDE.md rule 8.A — MVCC/visibility must be sound
 *	  or fail-closed within this spec):
 *	    - A reader at read_scn == commit_scn still needs the pre-image of that
 *	      version, so the comparison is STRICT '<' (equality => retained).
 *	    - An UNKNOWN (InvalidScn) commit_scn on a COMMITTED slot cannot be
 *	      proven below the horizon, so it is RETAINED (never recycled on a
 *	      guess).
 *	    - ABORTED versions are invisible to every read_scn (abort already
 *	      rolled the row back in place; CR rebuilds committed history only), so
 *	      no reader needs their undo => immediately recyclable (C7).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-3.12-retention-horizon.md (§2, §3 C3/C5/C7, D2/D3)
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_undo_retention.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cluster/cluster_scn.h"
#include "cluster/cluster_tt_slot.h"
#include "cluster/cluster_undo_retention.h"
#include "cluster/cluster_undo_segment.h"


/*
 * cluster_tt_slot_recyclable
 *
 *	Decide whether a TT-slot allocator entry may be recycled under the current
 *	retention horizon.  See header / file-banner contract.
 */
bool
cluster_tt_slot_recyclable(uint8 cts_status, SCN commit_scn, SCN horizon)
{
	/* C7: aborted versions are invisible to any read_scn -> always recyclable. */
	if (cts_status == CTS_ABORTED)
		return true;

	/* Only COMMITTED slots are gated; ACTIVE (in-flight) / FREE are not. */
	if (cts_status != CTS_COMMITTED)
		return false;

	/*
	 * InvalidScn horizon == cluster disabled (no live cluster reader can
	 * exist) -> no retention constraint, recycle freely.
	 */
	if (!SCN_VALID(horizon))
		return true;

	/*
	 * rule 8.A: a COMMITTED slot whose commit_scn is unresolved cannot be
	 * shown to be below the horizon -> fail-closed (retain).
	 */
	if (!SCN_VALID(commit_scn))
		return false;

	/* Strict '<': commit_scn == horizon is still needed by the oldest reader. */
	return scn_time_cmp(commit_scn, horizon) < 0;
}


/*
 * cluster_undo_segment_recyclable
 *
 *	Decide whether an undo segment may be recycled under the current horizon.
 *	Precondition: SEGMENT_COMMITTED (C5).  The segment's retention watermark is
 *	the max commit_scn over the COMMITTED on-disk TT slots in its header; the
 *	segment is recyclable only when that watermark is strictly below horizon.
 */
bool
cluster_undo_segment_recyclable(const struct UndoSegmentHeaderData *hdr, SCN horizon)
{
	SCN watermark = InvalidScn;
	bool saw_unresolved_committed = false;
	int i;

	if (hdr == NULL)
		return false;

	/*
	 * C5: only a SEGMENT_COMMITTED segment participates.  ALLOCATED / ACTIVE /
	 * FULL-but-ACTIVE (fullness is a flag, state stays ACTIVE) / RECYCLABLE are
	 * never selected here -- an "empty" ACTIVE segment is one being written,
	 * not a recycle candidate.
	 */
	if (hdr->segment_state != SEGMENT_COMMITTED)
		return false;

	/* InvalidScn horizon == cluster disabled -> no retention constraint. */
	if (!SCN_VALID(horizon))
		return true;

	for (i = 0; i < TT_SLOTS_PER_SEGMENT; i++) {
		const TTSlot *s = &hdr->tt_slots[i];

		if (s->status != TT_SLOT_COMMITTED)
			continue;
		if (!SCN_VALID(s->commit_scn)) {
			/* rule 8.A: unresolved committed slot -> retain whole segment. */
			saw_unresolved_committed = true;
			continue;
		}
		if (!SCN_VALID(watermark) || scn_time_cmp(s->commit_scn, watermark) > 0)
			watermark = s->commit_scn;
	}

	if (saw_unresolved_committed)
		return false;

	/* SEGMENT_COMMITTED with no live committed slot -> nothing to retain. */
	if (!SCN_VALID(watermark))
		return true;

	return scn_time_cmp(watermark, horizon) < 0;
}
