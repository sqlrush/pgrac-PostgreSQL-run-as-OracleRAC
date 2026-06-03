/*-------------------------------------------------------------------------
 *
 * test_cluster_retention.c
 *	  pgrac spec-3.12 D6 — retention horizon pure-predicate tests.
 *
 *	  Exercises the two pure judgement helpers that drive the
 *	  own-instance retention gate (spec-3.12 D2/D3):
 *	    cluster_tt_slot_recyclable()     — TT-slot allocator status gate
 *	    cluster_undo_segment_recyclable() — undo-segment header gate
 *
 *	  Both are pure (no shmem / no LWLock / no I/O); the only external
 *	  symbol they reference is scn_time_cmp(), which this binary stubs
 *	  with the same local-scn-only ordering the real comparator uses
 *	  (cluster_scn.c is covered by test_cluster_scn).
 *
 *	  Coverage (spec-3.12 §4.1):
 *	    U1  COMMITTED + commit_scn < horizon            -> recyclable
 *	    U2  COMMITTED + commit_scn > horizon            -> retained
 *	    U3  ABORTED (any commit_scn)                    -> recyclable (C7)
 *	    U4  ACTIVE / FREE                               -> not recyclable
 *	    U7  COMMITTED + commit_scn == horizon (strict <) -> retained
 *	    U8  InvalidScn horizon (cluster disabled)       -> recyclable
 *	    U8b COMMITTED + InvalidScn commit_scn (rule 8.A) -> retained
 *	    U5  SEGMENT_COMMITTED watermark < / == / > horizon + empty
 *	    U10 SEGMENT_ALLOCATED/ACTIVE/FULL-but-ACTIVE precondition
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-3.12-retention-horizon.md (§4.1, D6)
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_retention.c
 *
 *-------------------------------------------------------------------------
 */
#define USE_PGRAC_CLUSTER 1

#include "postgres.h"

#include <string.h>

#include "cluster/cluster_scn.h"
#include "cluster/cluster_tt_slot.h"
#include "cluster/cluster_undo_retention.h"
#include "cluster/cluster_undo_segment.h"

#undef printf
#undef fprintf
#undef snprintf

#include "unit_test.h"


UT_DEFINE_GLOBALS();


/*
 * scn_time_cmp stub — mirrors the real comparator's contract: visibility
 * ordering uses local_scn only (node_id bits are masked off).  All test SCNs
 * use node 0, so scn_local(v) == v.
 */
int
scn_time_cmp(SCN a, SCN b)
{
	uint64 la = scn_local(a);
	uint64 lb = scn_local(b);

	if (la < lb)
		return -1;
	if (la > lb)
		return 1;
	return 0;
}


/* Build a node-0 SCN whose local value is v (v >= 1 is a real SCN). */
static inline SCN
mk_scn(uint64 v)
{
	return (SCN)v;
}


/* Zero a segment header and set its lifecycle state. */
static void
init_header(UndoSegmentHeaderData *hdr, uint8 state)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->segment_state = state;
	hdr->tt_slots_count = TT_SLOTS_PER_SEGMENT;
}


/* Put one COMMITTED on-disk slot with commit_scn into slot[idx]. */
static void
set_committed_slot(UndoSegmentHeaderData *hdr, int idx, SCN commit_scn)
{
	hdr->tt_slots[idx].status = TT_SLOT_COMMITTED;
	hdr->tt_slots[idx].commit_scn = commit_scn;
}


/* ===== U1-U4, U7, U8: cluster_tt_slot_recyclable ===== */

UT_TEST(test_u1_committed_below_horizon_recyclable)
{
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_COMMITTED, mk_scn(5), mk_scn(10)), 1);
}

UT_TEST(test_u2_committed_above_horizon_retained)
{
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_COMMITTED, mk_scn(15), mk_scn(10)), 0);
}

UT_TEST(test_u3_aborted_always_recyclable)
{
	/* C7: ABORTED is invisible to any read_scn; commit_scn ignored. */
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_ABORTED, mk_scn(999), mk_scn(10)), 1);
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_ABORTED, InvalidScn, mk_scn(10)), 1);
}

UT_TEST(test_u4_active_free_not_recyclable)
{
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_ACTIVE, mk_scn(5), mk_scn(10)), 0);
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_FREE, mk_scn(5), mk_scn(10)), 0);
}

UT_TEST(test_u7_committed_equal_horizon_retained)
{
	/* horizon == min active read_scn; a reader at read_scn == commit_scn
	 * needs the pre-image -> strict '<' means equality is retained. */
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_COMMITTED, mk_scn(10), mk_scn(10)), 0);
}

UT_TEST(test_u8_invalid_horizon_recyclable)
{
	/* InvalidScn horizon == cluster disabled / no retention constraint. */
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_COMMITTED, mk_scn(5), InvalidScn), 1);
}

UT_TEST(test_u8b_committed_invalid_commit_scn_retained)
{
	/* rule 8.A: a COMMITTED slot whose commit_scn is unresolved cannot be
	 * proven below the horizon -> fail-closed (retain). */
	UT_ASSERT_EQ((int)cluster_tt_slot_recyclable(CTS_COMMITTED, InvalidScn, mk_scn(10)), 0);
}


/* ===== U5, U10: cluster_undo_segment_recyclable ===== */

UT_TEST(test_u5_segment_committed_watermark_below_horizon)
{
	UndoSegmentHeaderData hdr;

	init_header(&hdr, SEGMENT_COMMITTED);
	set_committed_slot(&hdr, 0, mk_scn(3));
	set_committed_slot(&hdr, 1, mk_scn(7)); /* watermark = max = 7 */
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 1);
}

UT_TEST(test_u5_segment_committed_watermark_at_or_above_horizon)
{
	UndoSegmentHeaderData hdr;

	init_header(&hdr, SEGMENT_COMMITTED);
	set_committed_slot(&hdr, 0, mk_scn(3));
	set_committed_slot(&hdr, 1, mk_scn(10)); /* watermark = 10 == horizon */
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);

	set_committed_slot(&hdr, 2, mk_scn(20)); /* watermark = 20 > horizon */
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);
}

UT_TEST(test_u5_segment_committed_no_committed_slot_recyclable)
{
	UndoSegmentHeaderData hdr;

	/* SEGMENT_COMMITTED with no live COMMITTED slot -> watermark absent ->
	 * recyclable. */
	init_header(&hdr, SEGMENT_COMMITTED);
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 1);
}

UT_TEST(test_u5_segment_committed_invalid_commit_scn_retained)
{
	UndoSegmentHeaderData hdr;

	/* rule 8.A: a COMMITTED-status on-disk slot with InvalidScn commit_scn
	 * (partial write) cannot be resolved -> retain the whole segment. */
	init_header(&hdr, SEGMENT_COMMITTED);
	hdr.tt_slots[0].status = TT_SLOT_COMMITTED;
	hdr.tt_slots[0].commit_scn = InvalidScn;
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);
}

UT_TEST(test_u8_segment_invalid_horizon_recyclable)
{
	UndoSegmentHeaderData hdr;

	init_header(&hdr, SEGMENT_COMMITTED);
	set_committed_slot(&hdr, 0, mk_scn(100)); /* even high watermark */
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, InvalidScn), 1);
}

UT_TEST(test_u10_segment_non_committed_state_never_recyclable)
{
	UndoSegmentHeaderData hdr;

	/* Even with zero committed slots (which would look "empty"), any state
	 * other than SEGMENT_COMMITTED must not be recyclable (C5/U10): it may
	 * be actively written or freshly allocated. */
	init_header(&hdr, SEGMENT_ALLOCATED);
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);

	init_header(&hdr, SEGMENT_ACTIVE);
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);

	/* FULL-but-ACTIVE: flag set, state still ACTIVE. */
	init_header(&hdr, SEGMENT_ACTIVE);
	hdr.segment_flags |= UNDO_SEGMENT_FLAG_FULL;
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);

	init_header(&hdr, SEGMENT_RECYCLABLE);
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(&hdr, mk_scn(10)), 0);
}

UT_TEST(test_u10_segment_null_header_not_recyclable)
{
	UT_ASSERT_EQ((int)cluster_undo_segment_recyclable(NULL, mk_scn(10)), 0);
}


int
main(void)
{
	UT_RUN(test_u1_committed_below_horizon_recyclable);
	UT_RUN(test_u2_committed_above_horizon_retained);
	UT_RUN(test_u3_aborted_always_recyclable);
	UT_RUN(test_u4_active_free_not_recyclable);
	UT_RUN(test_u7_committed_equal_horizon_retained);
	UT_RUN(test_u8_invalid_horizon_recyclable);
	UT_RUN(test_u8b_committed_invalid_commit_scn_retained);
	UT_RUN(test_u5_segment_committed_watermark_below_horizon);
	UT_RUN(test_u5_segment_committed_watermark_at_or_above_horizon);
	UT_RUN(test_u5_segment_committed_no_committed_slot_recyclable);
	UT_RUN(test_u5_segment_committed_invalid_commit_scn_retained);
	UT_RUN(test_u8_segment_invalid_horizon_recyclable);
	UT_RUN(test_u10_segment_non_committed_state_never_recyclable);
	UT_RUN(test_u10_segment_null_header_not_recyclable);

	return ut_failed_count == 0 ? 0 : 1;
}
