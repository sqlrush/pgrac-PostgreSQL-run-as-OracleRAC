/*-------------------------------------------------------------------------
 *
 * cluster_itl.c
 *	  pgrac cluster ITL slot read-only helper — extract TT slot ref
 *	  from a heap page's ITL array (PD_HAS_ITL special area).
 *
 *	  spec-3.1 D4 (NEW).
 *
 *	  This is foundation read-only access only.  No mutation of the
 *	  ITL slot or the page;  no cleanout write-back;  no TT slot
 *	  allocation.  spec-3.4 will activate ITL writable path
 *	  (commit_scn persistence + delayed cleanout);  this file ships
 *	  the read side that spec-3.2 visibility code consumes.
 *
 *	  See cluster_itl.h for the public contract.
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-3.1-cluster-xid-status-foundation.md
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_itl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufpage.h"
#include "cluster/cluster_itl.h"
#include "cluster/cluster_itl_slot.h"
#include "cluster/cluster_scn.h"
#include "cluster/cluster_tt_slot.h"

#ifdef USE_PGRAC_CLUSTER

bool
cluster_itl_get_tt_ref(Page page, uint8 itl_slot_idx, ClusterUndoTTSlotRef *ref)
{
	const ClusterItlSlotData *slot;
	const ClusterItlSlotData *slots;

	if (page == NULL || ref == NULL)
		return false;

	/* Page must declare it carries an ITL special area. */
	if (!PageHasItl(page))
		return false;

	/* Index range guard.  CLUSTER_ITL_INITRANS_DEFAULT is the
	 * fixed spec-1.5 placeholder count; spec-3.4 may make this
	 * per-table dynamic and at that point this guard becomes the
	 * page-derived bound. */
	if (itl_slot_idx >= CLUSTER_ITL_INITRANS_DEFAULT)
		return false;

	slots = ClusterPageGetItlSlots(page);
	slot = &slots[itl_slot_idx];

	/* Empty / free slot — no TT binding to report. */
	if (slot->flags == ITL_FLAG_FREE)
		return false;

	/*
	 * Fill the read-only ref.  spec-1.5 ITL slot layout does not yet
	 * carry origin_node_id / undo_segment_id / tt_slot_id on disk
	 * (those land in spec-3.4 ITL writable activation).  spec-3.1 D4
	 * reports the physical xid + commit_scn for the slot and zeros the
	 * origin/segment/tt_slot triple; visibility code (spec-3.2)
	 * decides how to combine with caller-side origin knowledge when
	 * constructing the ClusterTTStatusKey.
	 */
	ref->origin_node_id = 0;
	ref->undo_segment_id = 0;
	ref->tt_slot_id = 0;
	ref->cluster_epoch = 0;
	ref->local_xid = slot->xid;
	ref->cached_commit_scn = slot->commit_scn;
	ref->has_cached_status = (slot->flags == ITL_FLAG_COMMITTED && SCN_VALID(slot->commit_scn));
	memset(ref->_padding, 0, sizeof(ref->_padding));

	return true;
}

#else /* !USE_PGRAC_CLUSTER */

bool
cluster_itl_get_tt_ref(Page page, uint8 itl_slot_idx, ClusterUndoTTSlotRef *ref)
{
	(void)page;
	(void)itl_slot_idx;
	(void)ref;
	return false;
}

#endif /* USE_PGRAC_CLUSTER */
