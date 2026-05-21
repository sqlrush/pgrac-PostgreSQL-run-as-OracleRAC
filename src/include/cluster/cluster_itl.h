/*-------------------------------------------------------------------------
 *
 * cluster_itl.h
 *	  pgrac cluster ITL slot read-only helpers — extract TT slot ref
 *	  from a heap page's ITL array.
 *
 *	  spec-3.1 D4 (NEW).
 *
 *	  Backend-only header (depends on storage/bufpage.h Page type;
 *	  kept out of frontend-safe cluster_tt_slot.h per spec-3.1 §1.4).
 *
 *	  Read-only contract:
 *	    - cluster_itl_get_tt_ref MUST NOT mutate the page.
 *	    - PD_HAS_ITL=false or invalid slot index → returns false, ref
 *	      untouched.
 *	    - If ITL slot has cached commit_scn from a prior cleanout, the
 *	      ref carries it as `cached_commit_scn` (read-only;  D4 does NOT
 *	      write back).
 *
 *	  spec-3.1 v1.0 FROZEN scope (D4):
 *	    - read-only ITL reader;  NO commit_scn persistent write (推
 *	      spec-3.4 delayed cleanout);  NO TT slot allocation (推
 *	      spec-3.4);  NO HeapTupleSatisfiesMVCC cluster-path activation
 *	      (推 spec-3.2).
 *	    - origin_node / undo_segment_id / tt_slot_id are NOT yet stored
 *	      in the ITL slot on-disk format (spec-1.5 placeholder layout);
 *	      D4 fills them from the helper's caller-side knowledge and the
 *	      provisional in-memory mint (spec-3.1 v0.4 N6).  Real persistent
 *	      origin/segment/slot landing requires spec-3.4 ITL slot writable
 *	      activation.
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
 *	  src/include/cluster/cluster_itl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_ITL_H
#define CLUSTER_ITL_H

#include "c.h"
#include "storage/bufpage.h"		 /* Page typedef */
#include "cluster/cluster_tt_slot.h" /* ClusterUndoTTSlotRef */

/*
 * cluster_itl_get_tt_ref -- read ITL slot at `itl_slot_idx` and fill a
 * ClusterUndoTTSlotRef descriptor.
 *
 * Returns:
 *	  true   ITL slot is valid and `*ref` is filled.
 *	  false  PD_HAS_ITL is not set / itl_slot_idx is out of range /
 *	         slot is empty (ITL_FLAG_FREE).  `*ref` untouched.
 *
 * The page is not modified.
 *
 * Caveat (spec-3.1 v1.0):  origin_node_id / undo_segment_id /
 * tt_slot_id fields in the on-disk ITL slot are spec-1.5 placeholders
 * (zero).  D4 reports what is physically present; visibility consumers
 * (spec-3.2) decide how to combine with caller-side origin knowledge
 * when constructing a ClusterTTStatusKey for cluster_tt_status_lookup_exact.
 */
extern bool cluster_itl_get_tt_ref(Page page, uint8 itl_slot_idx, ClusterUndoTTSlotRef *ref);

#endif /* CLUSTER_ITL_H */
