/*-------------------------------------------------------------------------
 *
 * test_cluster_xlog.c
 *	  Compile-time + link-time invariants for the spec-1.19 WAL Page
 *	  Header xlp_thread_id / xlp_cluster_flags placeholder fields.
 *
 *	  These invariants guard the on-disk byte layout at the
 *	  cluster_unit layer (no PG postmaster needed) so any future struct
 *	  field reorder / unintended XLogPageHeaderData layout change is
 *	  caught before the bigger cluster_tap suite is exercised.
 *
 *	  Spec: spec-1.19-wal-page-header-thread-id.md §1.2 Deliverable 8
 *	        + §4.1 (3 项 cluster_unit 断言)
 *	  Design: docs/wal-record-format-design.md §5.1
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_xlog.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Includes "postgres.h" so XLogPageHeaderData's transitive PG-
 *	  internal types resolve.  PG headers are not actually called at
 *	  runtime -- this binary only reads sizeof / offsetof at compile
 *	  time and exercises the inline helper from cluster_xlog.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stddef.h> /* offsetof */

#include "access/xlog_internal.h"
#include "cluster/cluster_xlog.h"

/* Drop PG's port.h printf -> pg_printf override; unit_test.h uses
 * stdlib printf and we don't link libpgport in this test binary. */
#undef printf

#include "unit_test.h"


UT_DEFINE_GLOBALS();


/*
 * spec-1.19 Q1=A invariant: the placeholder fields reuse the existing
 * MAXALIGN tail padding so on-disk layout stays 24 bytes.  This duplicates
 * the StaticAssertDecl in xlog_internal.h at the test layer so a CI run
 * surfaces the failure in cluster_unit (faster than waiting for a
 * full rebuild).
 */
UT_TEST(test_spec119_xlog_page_header_size_is_24_bytes)
{
	UT_ASSERT_EQ(sizeof(XLogPageHeaderData), (size_t)24);
}

UT_TEST(test_spec119_xlp_thread_id_field_offset_is_20)
{
	UT_ASSERT_EQ(offsetof(XLogPageHeaderData, xlp_thread_id), (size_t)20);
}

UT_TEST(test_spec119_xlp_cluster_flags_field_offset_is_22)
{
	UT_ASSERT_EQ(offsetof(XLogPageHeaderData, xlp_cluster_flags), (size_t)22);
}

UT_TEST(test_spec119_xlp_thread_id_field_size_is_2)
{
	XLogPageHeaderData h;

	UT_ASSERT_EQ(sizeof(h.xlp_thread_id), (size_t)2);
}

UT_TEST(test_spec119_xlp_cluster_flags_field_size_is_2)
{
	XLogPageHeaderData h;

	UT_ASSERT_EQ(sizeof(h.xlp_cluster_flags), (size_t)2);
}

/*
 * Q2 v0.2 sentinel constants invariant.  Stage 2+ feature-034 must NOT
 * change these values.
 */
UT_TEST(test_spec119_sentinel_constants)
{
	UT_ASSERT_EQ((unsigned)XLP_THREAD_ID_LEGACY, 0u);
	UT_ASSERT_EQ((unsigned)XLP_THREAD_ID_FIRST_REAL, 1u);
	UT_ASSERT_EQ((unsigned)XLP_THREAD_ID_MAX_REAL, 0xFFFEu);
	UT_ASSERT_EQ((unsigned)XLP_THREAD_ID_INVALID, 0xFFFFu);
	UT_ASSERT_EQ((unsigned)XLP_CLUSTER_FLAGS_RESERVED, 0u);
}

/*
 * Stage 1 validator helper: returns true iff tid == LEGACY (0).
 */
UT_TEST(test_spec119_validate_thread_id_helper_legacy_passes)
{
	UT_ASSERT(cluster_xlog_validate_page_header_thread_id(XLP_THREAD_ID_LEGACY));
}

UT_TEST(test_spec119_validate_thread_id_helper_first_real_fails)
{
	UT_ASSERT(!cluster_xlog_validate_page_header_thread_id(XLP_THREAD_ID_FIRST_REAL));
}

UT_TEST(test_spec119_validate_thread_id_helper_invalid_fails)
{
	UT_ASSERT(!cluster_xlog_validate_page_header_thread_id(XLP_THREAD_ID_INVALID));
}


/*
 * Hardening v1.0.1 P2-2 (codex review 2026-05-05): negative-path
 * coverage for the Stage 1 invariant predicate that the xlogreader
 * validator hook delegates to.  Without these tests, an accidental
 * delete or condition flip in
 * cluster_xlog_validate_page_header_stage1_invariant (cluster_xlog.h)
 * or in the XLogReaderValidatePageHeader call site (xlogreader.c)
 * would not be caught by CI -- the inline thread_id helper alone did
 * not exercise the cluster_flags branch.
 *
 * Four quadrants cover the full (thread_id × cluster_flags) cartesian:
 *   (LEGACY,        RESERVED)   -> true   passes
 *   (LEGACY,        non-zero)   -> false  rejects on cluster_flags
 *   (non-zero,      RESERVED)   -> false  rejects on thread_id
 *   (non-zero,      non-zero)   -> false  rejects on both
 */
UT_TEST(test_spec119_stage1_invariant_legacy_reserved_passes)
{
	UT_ASSERT(cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_LEGACY,
																 XLP_CLUSTER_FLAGS_RESERVED));
}

UT_TEST(test_spec119_stage1_invariant_legacy_nonzero_flags_fails)
{
	UT_ASSERT(
		!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_LEGACY, (uint16)0x0001));
	UT_ASSERT(
		!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_LEGACY, (uint16)0xFFFF));
}

UT_TEST(test_spec119_stage1_invariant_nonzero_thread_legacy_flags_fails)
{
	UT_ASSERT(!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_FIRST_REAL,
																  XLP_CLUSTER_FLAGS_RESERVED));
	UT_ASSERT(!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_MAX_REAL,
																  XLP_CLUSTER_FLAGS_RESERVED));
	UT_ASSERT(!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_INVALID,
																  XLP_CLUSTER_FLAGS_RESERVED));
}

UT_TEST(test_spec119_stage1_invariant_both_nonzero_fails)
{
	UT_ASSERT(!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_FIRST_REAL,
																  (uint16)0x0001));
	UT_ASSERT(
		!cluster_xlog_validate_page_header_stage1_invariant(XLP_THREAD_ID_INVALID, (uint16)0xFFFF));
}


int
main(void)
{
	UT_PLAN(13);

	/* Layout invariants (5) */
	UT_RUN(test_spec119_xlog_page_header_size_is_24_bytes);
	UT_RUN(test_spec119_xlp_thread_id_field_offset_is_20);
	UT_RUN(test_spec119_xlp_cluster_flags_field_offset_is_22);
	UT_RUN(test_spec119_xlp_thread_id_field_size_is_2);
	UT_RUN(test_spec119_xlp_cluster_flags_field_size_is_2);

	/* Sentinel constants invariant (1) */
	UT_RUN(test_spec119_sentinel_constants);

	/* thread_id helper (3) */
	UT_RUN(test_spec119_validate_thread_id_helper_legacy_passes);
	UT_RUN(test_spec119_validate_thread_id_helper_first_real_fails);
	UT_RUN(test_spec119_validate_thread_id_helper_invalid_fails);

	/* Stage 1 invariant predicate, full quadrant coverage (4) -- v1.0.1 P2-2 */
	UT_RUN(test_spec119_stage1_invariant_legacy_reserved_passes);
	UT_RUN(test_spec119_stage1_invariant_legacy_nonzero_flags_fails);
	UT_RUN(test_spec119_stage1_invariant_nonzero_thread_legacy_flags_fails);
	UT_RUN(test_spec119_stage1_invariant_both_nonzero_fails);

	UT_DONE();
	return ut_failed_count == 0 ? 0 : 1;
}
