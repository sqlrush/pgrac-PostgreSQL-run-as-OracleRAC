/*-------------------------------------------------------------------------
 *
 * test_cluster_scn.c
 *	  Compile-time + link-time invariants for the SCN typedef stub
 *	  introduced at stage 1.4.
 *
 *	  Stage 1.4 ships only the typedef + InvalidScn constant + SCN_VALID
 *	  + SCN_FORMAT macros to src/include/cluster/cluster_scn.h.
 *	  Spec-1.15 will overlay the encoding layer (8b node_id || 56b
 *	  local_scn) and the comparison contract (scn_time_cmp /
 *	  scn_total_cmp / scn_recovery_cmp) -- those are tested separately
 *	  when they land.  This binary covers only the stub invariants.
 *
 *	  Spec: spec-1.4-block-format-pageheader-scn.md §1.2 Deliverable 5
 *	        + §4.1 (5 项 cluster_unit 断言)
 *	  Design: docs/scn-protocol-design.md v1.1 §3.2
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_scn.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Each test_*.c is a standalone executable; see unit_test.h.
 *	  cluster_scn.h is header-only at stage 1.4 -- no .o file to link.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "cluster/cluster_scn.h"

/* Drop PG's port.h printf -> pg_printf override; unit_test.h uses
 * stdlib printf and we don't link libpgport in this test binary. */
#undef printf

#include "unit_test.h"


UT_DEFINE_GLOBALS();


UT_TEST(test_scn_typedef_size_is_8_bytes)
{
	/*
	 * The SCN typedef must be 8 bytes (uint64 alias).  Stage 1.15 will
	 * overlay 8b node_id + 56b local_scn encoding on top; the byte
	 * width is locked at stage 1.4 so on-disk format never changes.
	 */
	UT_ASSERT_EQ(sizeof(SCN), 8);
}


UT_TEST(test_invalid_scn_is_zero)
{
	/*
	 * spec-1.4 §8 Q2 = A locks InvalidScn = 0.  This matches PG's
	 * InvalidTransactionId convention; lets MemSet zero-init occupy
	 * the "not yet set" semantics naturally (PageInit relies on this).
	 */
	UT_ASSERT_EQ((int)InvalidScn, 0);
}


UT_TEST(test_scn_valid_macro_rejects_zero)
{
	SCN zero = 0;

	/* SCN_VALID(InvalidScn) must be false. */
	UT_ASSERT_EQ((int)SCN_VALID(InvalidScn), 0);

	/* Direct 0 must also fail. */
	UT_ASSERT_EQ((int)SCN_VALID(zero), 0);
}


UT_TEST(test_scn_valid_macro_accepts_nonzero)
{
	SCN one = 1;
	SCN big = (SCN)1 << 56; /* matches future SCN_NODE_ID_SHIFT */

	UT_ASSERT(SCN_VALID(one));
	UT_ASSERT(SCN_VALID(big));
}


UT_TEST(test_scn_format_macro_produces_nonempty_string)
{
	const char *fmt = SCN_FORMAT;
	SCN test_val = 12345;
	unsigned long arg = SCN_FORMAT_ARG(test_val);

	/*
	 * Format macros are used in elog / SRF output; verify they
	 * compile + produce something non-empty.  Cannot easily verify
	 * exact format without printf; just check the format string is
	 * non-NULL and non-empty.
	 */
	UT_ASSERT_NOT_NULL((void *)fmt);
	UT_ASSERT(strlen(fmt) > 0);

	/* SCN_FORMAT_ARG should compile + return same value cast to ulong. */
	UT_ASSERT_EQ((int)arg, 12345);
}


int
main(void)
{
	UT_PLAN(5);
	UT_RUN(test_scn_typedef_size_is_8_bytes);
	UT_RUN(test_invalid_scn_is_zero);
	UT_RUN(test_scn_valid_macro_rejects_zero);
	UT_RUN(test_scn_valid_macro_accepts_nonzero);
	UT_RUN(test_scn_format_macro_produces_nonempty_string);
	UT_DONE();
	return ut_failed_count == 0 ? 0 : 1;
}
