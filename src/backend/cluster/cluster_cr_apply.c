/*-------------------------------------------------------------------------
 *
 * cluster_cr_apply.c
 *	  pgrac CR inverse-apply helpers (spec-3.9 D4).
 *
 *	  Four pure-logic helpers that inverse-apply one undo record onto a
 *	  backend-local CR scratch page.  Driven by the chain walker in
 *	  cluster_cr.c.  See cluster_cr_apply.h for the per-helper contract.
 *
 *	  All four mutate the scratch page in place and return false on any
 *	  inconsistency (caller ereports data_corrupted — spec-3.9 I-fail-4).
 *	  None of them touch shared buffers, WAL, or hint bits beyond the
 *	  scratch page (spec-3.9 I-lock-4).
 *
 *	  Heap update/delete in PostgreSQL keep the old tuple physically in
 *	  place (only the header xmax / ctid / infomask change); the new
 *	  version goes elsewhere.  So inverse-update / inverse-delete restore
 *	  the full pre-change tuple image captured in the undo record over the
 *	  tuple bytes at target_offset — a length-preserving overwrite.
 *	  Inverse-insert removes the inserted line pointer (LP_UNUSED).
 *	  Inverse-ITL restores the lock-only tuple-header fields + ITL slot
 *	  fields captured in UndoItlPayload.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-3.9-own-instance-cr-block-construction.md (FROZEN v0.4)
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_cr_apply.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"

#include "cluster/cluster_cr_apply.h"
#include "cluster/cluster_itl_slot.h"


/*
 * Resolve and range-check the line pointer at target_offset on the scratch
 * page.  Returns the ItemId on success, NULL if the offset is out of range.
 */
static ItemId
cr_target_itemid(char *scratch_page, OffsetNumber off)
{
	Page page = (Page)scratch_page;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return NULL;
	return PageGetItemId(page, off);
}


/*
 * cluster_cr_apply_insert_inverse -- undo an INSERT: remove the inserted
 *	tuple by marking its line pointer LP_UNUSED.  A seqscan / index fetch
 *	skips LP_UNUSED, so the row correctly disappears from the CR image.
 *	(We do not rewind pd_lower: a trailing LP_UNUSED slot is harmless on a
 *	transient read-only image and keeps subsequent offsets stable.)
 */
bool
cluster_cr_apply_insert_inverse(char *scratch_page, const UndoRecordHeader *hdr,
								const UndoInsertPayload *payload)
{
	ItemId itemid = cr_target_itemid(scratch_page, hdr->target_offset);

	if (itemid == NULL || !ItemIdIsNormal(itemid))
		return false;

	/* Optional sanity: the recorded inserted length matches the live tuple. */
	if (payload->inserted_tuple_len != 0 && ItemIdGetLength(itemid) != payload->inserted_tuple_len)
		return false;

	ItemIdSetUnused(itemid);
	return true;
}


/*
 * Shared body for inverse-update / inverse-delete: overwrite the tuple at
 * target_offset with the captured pre-change full image.  The overwrite is
 * length-preserving (PG keeps the old tuple physically in place across an
 * update/delete), so the captured length must equal the live item length.
 */
static bool
cr_restore_full_image(char *scratch_page, OffsetNumber off, const char *image_bytes,
					  uint16 image_length)
{
	ItemId itemid = cr_target_itemid(scratch_page, off);
	char *item;

	if (itemid == NULL || !ItemIdIsNormal(itemid))
		return false;
	if (image_bytes == NULL || image_length == 0)
		return false;
	if (ItemIdGetLength(itemid) != image_length)
		return false; /* in-place overwrite must be length-preserving */

	item = (char *)PageGetItem((Page)scratch_page, itemid);
	memcpy(item, image_bytes, image_length);
	return true;
}


/*
 * cluster_cr_apply_update_inverse -- undo an UPDATE: restore the old tuple
 *	image (which had xmax invalid + original ctid) over the tuple bytes at
 *	target_offset.
 */
bool
cluster_cr_apply_update_inverse(char *scratch_page, const UndoRecordHeader *hdr,
								const UndoUpdatePayload *payload, const char *old_tuple_bytes,
								uint16 old_tuple_length)
{
	(void)payload; /* new_block/new_offset are not needed for the in-place restore */
	return cr_restore_full_image(scratch_page, hdr->target_offset, old_tuple_bytes,
								 old_tuple_length);
}


/*
 * cluster_cr_apply_delete_inverse -- undo a DELETE: restore the full
 *	pre-delete tuple image over the tuple bytes at target_offset (clearing
 *	the xmax / HEAP_XMAX_* the delete set, since the captured image predates
 *	the delete).
 */
bool
cluster_cr_apply_delete_inverse(char *scratch_page, const UndoRecordHeader *hdr,
								const UndoDeletePayload *payload, const char *full_tuple_bytes,
								uint16 full_tuple_length)
{
	(void)payload;
	return cr_restore_full_image(scratch_page, hdr->target_offset, full_tuple_bytes,
								 full_tuple_length);
}


/*
 * cluster_cr_apply_itl_inverse -- undo a lock-only ITL transition: restore
 *	the tuple-header fields the lock set (xmax / infomask / infomask2) and
 *	the affected ITL slot's fields (flags / commit_scn / undo_segment_head)
 *	from the UndoItlPayload captured prior state.
 */
bool
cluster_cr_apply_itl_inverse(char *scratch_page, const UndoRecordHeader *hdr,
							 const UndoItlPayload *payload)
{
	Page page = (Page)scratch_page;
	ItemId itemid;
	HeapTupleHeader htup;
	ClusterItlSlotData *slots;

	if (payload == NULL)
		return false;
	if (payload->itl_slot_idx >= CLUSTER_ITL_INITRANS_DEFAULT)
		return false;
	if (!PageHasItl(page))
		return false;

	/* Restore the tuple header the lock-only change touched. */
	itemid = cr_target_itemid(scratch_page, hdr->target_offset);
	if (itemid == NULL || !ItemIdIsNormal(itemid))
		return false;
	htup = (HeapTupleHeader)PageGetItem(page, itemid);
	HeapTupleHeaderSetXmax(htup, payload->prev_xmax);
	htup->t_infomask = payload->prev_infomask;
	htup->t_infomask2 = payload->prev_infomask2;

	/* Restore the ITL slot's captured prior state. */
	slots = ClusterPageGetItlSlots(page);
	slots[payload->itl_slot_idx].flags = payload->prev_flags;
	slots[payload->itl_slot_idx].commit_scn = payload->prev_commit_scn;
	slots[payload->itl_slot_idx].undo_segment_head = payload->prev_undo_segment_head;
	return true;
}
