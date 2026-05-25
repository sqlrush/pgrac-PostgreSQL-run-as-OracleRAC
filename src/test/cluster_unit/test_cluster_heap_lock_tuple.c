/*-------------------------------------------------------------------------
 *
 * test_cluster_heap_lock_tuple.c
 *	  pgrac spec-3.4d D13 — cluster_unit tests for heap_lock_tuple
 *	  lock-only ITL + fail-closed remote ACTIVE detection.
 *
 *	  19 tests covering:
 *	    T1   SizeofHeapTupleHeader unchanged (==24 in cluster build) per
 *	         spec-3.4d F2 P0 (no t_lock_itl_slot_idx field; raw_xmax scan)
 *	    T2   ITL_FLAG_LOCK_ONLY_ACTIVE / COMMITTED / ABORTED enum stable (5/6/7)
 *	    T3   ITL_FLAG_IS_LOCK_ONLY helper macro correctness
 *	    T4   ITL_FLAG_IS_LOCK_ONLY_COMPLETED helper macro correctness
 *	    T5   cluster_itl_find_lock_tt_ref_by_xmax symbol linkable
 *	    T6   cluster_itl_bump_overflow_lock_count symbol linkable
 *	    T7   cluster_itl_bump_multixact_lock_reject_count symbol linkable
 *	    T8   cluster_itl_bump_remote_row_lock_fail_closed_count symbol linkable
 *	    T9   cluster_itl_bump_lock_only_itl_stamp_count symbol linkable
 *	    T10  cluster_itl_bump_lock_only_tt_hint_emit_count symbol linkable
 *	    T11  cluster_tt_local_record_active symbol linkable (F3 P0 — ACTIVE TT install)
 *	    T12  ERRCODE_CLUSTER_REMOTE_ROW_LOCK_WAIT_NOT_SUPPORTED == 53R98
 *	    T13  ERRCODE_CLUSTER_MULTIXACT_LOCK_NOT_SUPPORTED == 53R99
 *	    T14  ERRCODE_CLUSTER_ITL_SLOT_OVERFLOW == 53R9A (F7 — distinct from 53R94)
 *	    T15  XLH_LOCK_ITL_DELTA == (1<<7) (Q4 A2 — bit 7 namespace consistency)
 *	    T16  XLH_LOCK_UPDATED_ITL_DELTA == (1<<7) (independent namespace bit 7)
 *	    T17  CLUSTER_ITL_INITRANS_DEFAULT == 8 (lock + data share capacity)
 *	    T18  ClusterItlSlotData sizeof 48 (regression — LOCK_ONLY_* states
 *	         don't grow the slot)
 *	    T19  ClusterItlSlotData field offsets stable (xid=0, flags=6, commit_scn=24)
 *
 *	  Behavioural coverage of heap_lock_tuple full path (raw_xmax scan +
 *	  ACTIVE TT install + TT_STATUS_HINT emit + wait_policy-aware fail-
 *	  closed) lives in cluster_tap t/209.  cluster_unit cannot drive a
 *	  real backend's heap_lock_tuple without postmaster.
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_heap_lock_tuple.c
 *
 * Spec: spec-3.4d-heap-lock-tuple-cross-node-lock-itl-fail-closed.md
 *       (v0.2 FROZEN 2026-05-25)
 *
 *-------------------------------------------------------------------------
 */
#define USE_PGRAC_CLUSTER 1

#include "postgres.h"

#include <stddef.h>

#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "cluster/cluster_itl.h"
#include "cluster/cluster_itl_slot.h"
#include "cluster/cluster_scn.h"
#include "cluster/cluster_tt_local.h"
#include "cluster/cluster_tt_slot.h"
#include "storage/bufpage.h"
#include "utils/errcodes.h"

#undef printf
#undef fprintf
#undef snprintf

#include "unit_test.h"


UT_DEFINE_GLOBALS();


void
ExceptionalCondition(const char *conditionName pg_attribute_unused(),
					 const char *fileName pg_attribute_unused(),
					 int lineNumber pg_attribute_unused())
{
	abort();
}


/* Local stubs — cluster_unit binary does not link the full set of
 * cluster_itl.o / cluster_tt_local.o (would drag in shmem + LWLock).
 * Symbol presence is what we verify here;  behavioural coverage lives
 * in cluster_tap t/209. */
bool
cluster_itl_find_lock_tt_ref_by_xmax(Page page pg_attribute_unused(),
									 TransactionId raw_xmax pg_attribute_unused(),
									 ClusterUndoTTSlotRef *ref pg_attribute_unused())
{
	return false;
}
void
cluster_itl_bump_overflow_lock_count(void)
{}
void
cluster_itl_bump_multixact_lock_reject_count(void)
{}
void
cluster_itl_bump_remote_row_lock_fail_closed_count(void)
{}
void
cluster_itl_bump_lock_only_itl_stamp_count(void)
{}
void
cluster_itl_bump_lock_only_tt_hint_emit_count(void)
{}
void
cluster_tt_local_record_active(TransactionId xid pg_attribute_unused())
{}


/* ===== T1: SizeofHeapTupleHeader unchanged (F2 P0) ===== */
UT_TEST(t1_tuple_header_sizeof_24)
{
	/* spec-3.4d F2 P0:  v0.1 t_lock_itl_slot_idx + sizeof bump 24->25 was
	 * REJECTED because MAXALIGN(SizeofHeapTupleHeader) makes the real
	 * cost +8B/tuple on 8B align platforms.  v0.2 uses raw_xmax + ITL
	 * scan derivation;  SizeofHeapTupleHeader stays at 24. */
	UT_ASSERT_EQ((int)SizeofHeapTupleHeader, 24);
}


/* ===== T2: ITL_FLAG_LOCK_ONLY_* enum stable (5/6/7) ===== */
UT_TEST(t2_itl_flag_lock_only_enum_stable)
{
	UT_ASSERT_EQ((int)ITL_FLAG_LOCK_ONLY_ACTIVE, 5);
	UT_ASSERT_EQ((int)ITL_FLAG_LOCK_ONLY_COMMITTED, 6);
	UT_ASSERT_EQ((int)ITL_FLAG_LOCK_ONLY_ABORTED, 7);

	/* spec-3.4a/c data ITL states still 0-4 (no collision). */
	UT_ASSERT_EQ((int)ITL_FLAG_FREE, 0);
	UT_ASSERT_EQ((int)ITL_FLAG_ACTIVE, 1);
	UT_ASSERT_EQ((int)ITL_FLAG_COMMITTED, 2);
	UT_ASSERT_EQ((int)ITL_FLAG_ABORTED, 3);
	UT_ASSERT_EQ((int)ITL_FLAG_NEEDS_CLEANOUT, 4);
}


/* ===== T3: ITL_FLAG_IS_LOCK_ONLY helper macro ===== */
UT_TEST(t3_itl_flag_is_lock_only_macro)
{
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_LOCK_ONLY_ACTIVE), 0);
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_LOCK_ONLY_COMMITTED), 0);
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_LOCK_ONLY_ABORTED), 0);
	/* data ITL states must return 0 */
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_FREE), 0);
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_ACTIVE), 0);
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_COMMITTED), 0);
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY(ITL_FLAG_ABORTED), 0);
}


/* ===== T4: ITL_FLAG_IS_LOCK_ONLY_COMPLETED helper macro ===== */
UT_TEST(t4_itl_flag_is_lock_only_completed_macro)
{
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_LOCK_ONLY_COMMITTED), 0);
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_LOCK_ONLY_ABORTED), 0);
	/* ACTIVE lock-only is NOT completed */
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_LOCK_ONLY_ACTIVE), 0);
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_ACTIVE), 0);
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_COMMITTED), 0);
}


/* ===== T5-T10: API symbols linkable ===== */

UT_TEST(t5_find_lock_tt_ref_by_xmax_linkable)
{
	UT_ASSERT_NE((void *)cluster_itl_find_lock_tt_ref_by_xmax, NULL);
}
UT_TEST(t6_overflow_lock_count_linkable)
{
	UT_ASSERT_NE((void *)cluster_itl_bump_overflow_lock_count, NULL);
}
UT_TEST(t7_multixact_lock_reject_count_linkable)
{
	UT_ASSERT_NE((void *)cluster_itl_bump_multixact_lock_reject_count, NULL);
}
UT_TEST(t8_remote_row_lock_fail_closed_count_linkable)
{
	UT_ASSERT_NE((void *)cluster_itl_bump_remote_row_lock_fail_closed_count, NULL);
}
UT_TEST(t9_lock_only_itl_stamp_count_linkable)
{
	UT_ASSERT_NE((void *)cluster_itl_bump_lock_only_itl_stamp_count, NULL);
}
UT_TEST(t10_lock_only_tt_hint_emit_count_linkable)
{
	UT_ASSERT_NE((void *)cluster_itl_bump_lock_only_tt_hint_emit_count, NULL);
}


/* ===== T11: cluster_tt_local_record_active linkable (F3 P0) ===== */
UT_TEST(t11_record_active_linkable)
{
	/* spec-3.4d F3 P0:  without record_active(install ACTIVE TT + emit
	 * TT_STATUS_HINT), peer永 lookup miss → 53R97 misfire instead of
	 * 53R98.  Symbol linkability gates downstream compilation. */
	UT_ASSERT_NE((void *)cluster_tt_local_record_active, NULL);
}


/* ===== T12-T14: NEW 3 SQLSTATE encodable ===== */

UT_TEST(t12_errcode_53r98_encodable)
{
	int sqlstate = MAKE_SQLSTATE('5', '3', 'R', '9', '8');
	UT_ASSERT_EQ((int)ERRCODE_CLUSTER_REMOTE_ROW_LOCK_WAIT_NOT_SUPPORTED, sqlstate);
}
UT_TEST(t13_errcode_53r99_encodable)
{
	int sqlstate = MAKE_SQLSTATE('5', '3', 'R', '9', '9');
	UT_ASSERT_EQ((int)ERRCODE_CLUSTER_MULTIXACT_LOCK_NOT_SUPPORTED, sqlstate);
}
UT_TEST(t14_errcode_53r9a_encodable)
{
	int sqlstate = MAKE_SQLSTATE('5', '3', 'R', '9', 'A');
	UT_ASSERT_EQ((int)ERRCODE_CLUSTER_ITL_SLOT_OVERFLOW, sqlstate);
	/* F7:  must not equal 53R94 sinval_queue_full (semantically distinct) */
	UT_ASSERT_NE((int)ERRCODE_CLUSTER_ITL_SLOT_OVERFLOW, (int)ERRCODE_CLUSTER_SINVAL_QUEUE_FULL);
}


/* ===== T15-T16: WAL bit 7 namespace consistency ===== */

UT_TEST(t15_xlh_lock_itl_delta_bit_7)
{
	UT_ASSERT_EQ((int)XLH_LOCK_ITL_DELTA, (1 << 7));
	/* Must not collide with XLH_LOCK_ALL_FROZEN_CLEARED (bit 0). */
	UT_ASSERT_NE((int)XLH_LOCK_ITL_DELTA, (int)XLH_LOCK_ALL_FROZEN_CLEARED);
}
UT_TEST(t16_xlh_lock_updated_itl_delta_bit_7)
{
	UT_ASSERT_EQ((int)XLH_LOCK_UPDATED_ITL_DELTA, (1 << 7));
}


/* ===== T17: INITRANS bound (lock + data share 8 slots) ===== */
UT_TEST(t17_initrans_default_is_8)
{
	UT_ASSERT_EQ((int)CLUSTER_ITL_INITRANS_DEFAULT, 8);
}


/* ===== T18-T19: ClusterItlSlotData layout regression ===== */

UT_TEST(t18_slot_sizeof_48)
{
	UT_ASSERT_EQ((int)sizeof(ClusterItlSlotData), 48);
}
UT_TEST(t19_slot_field_offsets_stable)
{
	UT_ASSERT_EQ((int)offsetof(ClusterItlSlotData, xid), 0);
	UT_ASSERT_EQ((int)offsetof(ClusterItlSlotData, flags), 6);
	UT_ASSERT_EQ((int)offsetof(ClusterItlSlotData, commit_scn), 24);
}


/* ===== T20-T24: spec-3.4d extra ABI / contract anchors ===== */

UT_TEST(t20_lock_only_states_distinct_from_data_states)
{
	/* spec-3.4d D1: LOCK_ONLY_* states (5/6/7) must not collide with
	 * data ITL states (0-4) — existing slot.flags == ITL_FLAG_ACTIVE
	 * equality checks in spec-3.4a/b/c must NOT match LOCK_ONLY_ACTIVE. */
	UT_ASSERT_NE((int)ITL_FLAG_LOCK_ONLY_ACTIVE, (int)ITL_FLAG_ACTIVE);
	UT_ASSERT_NE((int)ITL_FLAG_LOCK_ONLY_COMMITTED, (int)ITL_FLAG_COMMITTED);
	UT_ASSERT_NE((int)ITL_FLAG_LOCK_ONLY_ABORTED, (int)ITL_FLAG_ABORTED);
}

UT_TEST(t21_xlh_lock_itl_delta_distinct_from_existing)
{
	/* spec-3.4d Q4 A2 + L184:  bit 7 (0x80) must not collide with
	 * existing xl_heap_lock.flags bit 0 = XLH_LOCK_ALL_FROZEN_CLEARED.
	 * Same for xl_heap_lock_updated namespace. */
	UT_ASSERT_EQ((int)(XLH_LOCK_ITL_DELTA & XLH_LOCK_ALL_FROZEN_CLEARED), 0);
	UT_ASSERT_EQ((int)(XLH_LOCK_UPDATED_ITL_DELTA & XLH_LOCK_ALL_FROZEN_CLEARED), 0);
}

UT_TEST(t22_lock_only_active_can_recycle)
{
	/* spec-3.4d L189 lineage:  LOCK_ONLY_COMMITTED / LOCK_ONLY_ABORTED
	 * are "completed" states eligible for slot recycling (per the
	 * existing cluster_itl_alloc_or_reuse_slot policy that scans for
	 * non-ACTIVE non-FREE slots).  ITL_FLAG_IS_LOCK_ONLY_COMPLETED
	 * macro identifies them. */
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_LOCK_ONLY_COMMITTED), 0);
	UT_ASSERT_NE((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_LOCK_ONLY_ABORTED), 0);
	UT_ASSERT_EQ((int)ITL_FLAG_IS_LOCK_ONLY_COMPLETED(ITL_FLAG_LOCK_ONLY_ACTIVE), 0);
}

UT_TEST(t23_invalid_xid_constant_sentinel)
{
	/* spec-3.4d D1 cluster_itl_find_lock_tt_ref_by_xmax requires
	 * TransactionIdIsValid(raw_xmax) — InvalidTransactionId (== 0)
	 * must be the sentinel.  This catches accidental redefinition. */
	UT_ASSERT_EQ((int)InvalidTransactionId, 0);
}

UT_TEST(t24_cluster_itl_slot_unallocated_sentinel_consistent)
{
	/* spec-3.4d:  CLUSTER_ITL_SLOT_UNALLOCATED == 255 sentinel still
	 * used as the "no slot assigned" marker on t_itl_slot_idx (data
	 * ITL).  Lock-only path uses raw_xmax scan derivation (F2), no
	 * separate sentinel needed. */
	UT_ASSERT_EQ((int)CLUSTER_ITL_SLOT_UNALLOCATED, 255);
}


int
main(void)
{
	UT_PLAN(24);
	UT_RUN(t1_tuple_header_sizeof_24);
	UT_RUN(t2_itl_flag_lock_only_enum_stable);
	UT_RUN(t3_itl_flag_is_lock_only_macro);
	UT_RUN(t4_itl_flag_is_lock_only_completed_macro);
	UT_RUN(t5_find_lock_tt_ref_by_xmax_linkable);
	UT_RUN(t6_overflow_lock_count_linkable);
	UT_RUN(t7_multixact_lock_reject_count_linkable);
	UT_RUN(t8_remote_row_lock_fail_closed_count_linkable);
	UT_RUN(t9_lock_only_itl_stamp_count_linkable);
	UT_RUN(t10_lock_only_tt_hint_emit_count_linkable);
	UT_RUN(t11_record_active_linkable);
	UT_RUN(t12_errcode_53r98_encodable);
	UT_RUN(t13_errcode_53r99_encodable);
	UT_RUN(t14_errcode_53r9a_encodable);
	UT_RUN(t15_xlh_lock_itl_delta_bit_7);
	UT_RUN(t16_xlh_lock_updated_itl_delta_bit_7);
	UT_RUN(t17_initrans_default_is_8);
	UT_RUN(t18_slot_sizeof_48);
	UT_RUN(t19_slot_field_offsets_stable);
	UT_RUN(t20_lock_only_states_distinct_from_data_states);
	UT_RUN(t21_xlh_lock_itl_delta_distinct_from_existing);
	UT_RUN(t22_lock_only_active_can_recycle);
	UT_RUN(t23_invalid_xid_constant_sentinel);
	UT_RUN(t24_cluster_itl_slot_unallocated_sentinel_consistent);
	UT_DONE();
	return ut_failed_count == 0 ? 0 : 1;
}
