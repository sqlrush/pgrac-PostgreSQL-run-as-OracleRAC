/*-------------------------------------------------------------------------
 *
 * test_cluster_tt_slot.c
 *	  Compile-time + link-time invariants for the spec-1.20 Transaction
 *	  Table slot type definition.
 *
 *	  These invariants guard the on-disk byte layout at the
 *	  cluster_unit layer (no PG postmaster needed) so any future
 *	  TTSlot field reorder / unintended struct layout change is caught
 *	  before the bigger cluster_tap suite is exercised.
 *
 *	  Spec: spec-1.20-tt-slot-data-structure.md §4.1
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_tt_slot.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Includes "postgres.h" so TransactionId / SCN / UBA's transitive
 *	  PG-internal types resolve.  PG headers are not actually called at
 *	  runtime -- this binary only reads sizeof / offsetof at compile
 *	  time and exercises the inline helpers from cluster_tt_slot.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stddef.h> /* offsetof */
#include <string.h> /* memset */

#include "cluster/cluster_tt_slot.h"

/* Drop PG's port.h printf -> pg_printf override; unit_test.h uses
 * stdlib printf and we don't link libpgport in this test binary. */
#undef printf

#include "unit_test.h"


UT_DEFINE_GLOBALS();


/*
 * On-disk layout invariants -- five tests cover sizeof + four field
 * offsets.  Duplicate the StaticAssertDecl in cluster_tt_slot.h at the
 * test layer so a CI run surfaces the failure in cluster_unit faster
 * than waiting for a full backend rebuild.
 */
UT_TEST(test_spec120_ttslot_size_is_32_bytes)
{
	UT_ASSERT_EQ(sizeof(TTSlot), (size_t)32);
}

UT_TEST(test_spec120_ttslot_xid_offset_is_0)
{
	UT_ASSERT_EQ(offsetof(TTSlot, xid), (size_t)0);
}

UT_TEST(test_spec120_ttslot_wrap_offset_is_4)
{
	UT_ASSERT_EQ(offsetof(TTSlot, wrap), (size_t)4);
}

UT_TEST(test_spec120_ttslot_status_offset_is_6)
{
	UT_ASSERT_EQ(offsetof(TTSlot, status), (size_t)6);
}

UT_TEST(test_spec120_ttslot_flags_offset_is_7)
{
	UT_ASSERT_EQ(offsetof(TTSlot, flags), (size_t)7);
}

UT_TEST(test_spec120_ttslot_commit_scn_offset_is_8)
{
	UT_ASSERT_EQ(offsetof(TTSlot, commit_scn), (size_t)8);
}

UT_TEST(test_spec120_ttslot_first_undo_block_offset_is_16)
{
	UT_ASSERT_EQ(offsetof(TTSlot, first_undo_block), (size_t)16);
}


/*
 * Sentinel constants invariant.  Stage 2+ extensions MUST NOT change
 * these values.
 */
UT_TEST(test_spec120_status_enum_values)
{
	UT_ASSERT_EQ((unsigned)TT_SLOT_UNUSED, 0u);
	UT_ASSERT_EQ((unsigned)TT_SLOT_ACTIVE, 1u);
	UT_ASSERT_EQ((unsigned)TT_SLOT_COMMITTED, 2u);
	UT_ASSERT_EQ((unsigned)TT_SLOT_ABORTED, 3u);
	UT_ASSERT_EQ((unsigned)TT_SLOT_RECYCLABLE, 4u);
	UT_ASSERT_EQ((unsigned)TT_SLOT_INVALID, 0xFFu);
}

UT_TEST(test_spec120_sentinel_constants)
{
	UT_ASSERT_EQ((unsigned)TT_WRAP_INITIAL, 0u);
	UT_ASSERT_EQ((unsigned)TT_WRAP_MAX, 0xFFFEu);
	UT_ASSERT_EQ((unsigned)TT_WRAP_INVALID, 0xFFFFu);
	UT_ASSERT_EQ((unsigned)TT_FLAGS_RESERVED, 0u);
	UT_ASSERT_EQ(TT_SLOTS_PER_SEGMENT, 48);
}


/*
 * Inline helpers -- four-quadrant coverage on TTSlot_is_unused +
 * TTSlot_is_committed across the five real status values.  Mirrors
 * the spec-1.19 P2-2 hardening pattern: helper logic is the testable
 * surface that future activation paths will rely on, so cover all
 * meaningful states explicitly.
 */
UT_TEST(test_spec120_helper_is_unused_zero_init_passes)
{
	TTSlot slot;

	/*
	 * Zero-init slot lands at status == UNUSED == 0; this is the
	 * single most important invariant for safe undo segment header
	 * memset behaviour.
	 */
	memset(&slot, 0, sizeof(slot));
	UT_ASSERT(TTSlot_is_unused(&slot));
	UT_ASSERT(!TTSlot_is_committed(&slot));
}

UT_TEST(test_spec120_helper_is_unused_recyclable_passes)
{
	TTSlot slot;

	memset(&slot, 0, sizeof(slot));
	slot.status = TT_SLOT_RECYCLABLE;
	UT_ASSERT(TTSlot_is_unused(&slot));
	UT_ASSERT(!TTSlot_is_committed(&slot));
}

UT_TEST(test_spec120_helper_is_unused_active_fails)
{
	TTSlot slot;

	memset(&slot, 0, sizeof(slot));
	slot.status = TT_SLOT_ACTIVE;
	UT_ASSERT(!TTSlot_is_unused(&slot));
	UT_ASSERT(!TTSlot_is_committed(&slot));
}

UT_TEST(test_spec120_helper_is_committed_requires_status_and_scn)
{
	TTSlot slot;

	memset(&slot, 0, sizeof(slot));

	/*
	 * COMMITTED status with InvalidScn (e.g., crash mid-write before
	 * commit_scn populated) MUST return false -- the helper contract
	 * is "committed AND commit_scn populated", not status alone.
	 */
	slot.status = TT_SLOT_COMMITTED;
	slot.commit_scn = InvalidScn;
	UT_ASSERT(!TTSlot_is_unused(&slot));
	UT_ASSERT(!TTSlot_is_committed(&slot));

	/* COMMITTED + valid commit_scn = true (the only positive case). */
	slot.commit_scn = (SCN)42;
	UT_ASSERT(!TTSlot_is_unused(&slot));
	UT_ASSERT(TTSlot_is_committed(&slot));

	/* ABORTED with any commit_scn = false. */
	slot.status = TT_SLOT_ABORTED;
	slot.commit_scn = (SCN)42;
	UT_ASSERT(!TTSlot_is_unused(&slot));
	UT_ASSERT(!TTSlot_is_committed(&slot));
}


/*
 * UBA dependency on spec-1.5: a zero-init TTSlot must have an
 * InvalidUba in its first_undo_block field, validated through the
 * spec-1.5 helper UBA_is_invalid().  Catches accidental UBA encoding
 * change at spec-1.5 layer that would silently invalidate the
 * TTSlot zero-init contract.
 */
UT_TEST(test_spec120_uba_is_invalid_on_zero_init_slot)
{
	TTSlot slot;

	memset(&slot, 0, sizeof(slot));
	UT_ASSERT(UBA_is_invalid(slot.first_undo_block));
}


int
main(void)
{
	UT_PLAN(14);

	/* Layout invariants (7) */
	UT_RUN(test_spec120_ttslot_size_is_32_bytes);
	UT_RUN(test_spec120_ttslot_xid_offset_is_0);
	UT_RUN(test_spec120_ttslot_wrap_offset_is_4);
	UT_RUN(test_spec120_ttslot_status_offset_is_6);
	UT_RUN(test_spec120_ttslot_flags_offset_is_7);
	UT_RUN(test_spec120_ttslot_commit_scn_offset_is_8);
	UT_RUN(test_spec120_ttslot_first_undo_block_offset_is_16);

	/* Sentinel + enum invariants (2) */
	UT_RUN(test_spec120_status_enum_values);
	UT_RUN(test_spec120_sentinel_constants);

	/* Inline helpers, four-quadrant coverage (4) */
	UT_RUN(test_spec120_helper_is_unused_zero_init_passes);
	UT_RUN(test_spec120_helper_is_unused_recyclable_passes);
	UT_RUN(test_spec120_helper_is_unused_active_fails);
	UT_RUN(test_spec120_helper_is_committed_requires_status_and_scn);

	/* UBA dependency (1) -- skipped from PLAN count via SKIP for now? No, listed below. */
	UT_RUN(test_spec120_uba_is_invalid_on_zero_init_slot);

	UT_DONE();
	return ut_failed_count == 0 ? 0 : 1;
}
