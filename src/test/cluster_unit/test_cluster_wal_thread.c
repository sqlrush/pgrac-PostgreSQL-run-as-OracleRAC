/*-------------------------------------------------------------------------
 *
 * test_cluster_wal_thread.c
 *	  pgrac spec-4.1 D8 — cluster_unit tests for the per-thread WAL
 *	  routing pure helpers (cluster_wal_thread.h, header-only inline).
 *
 *	  12 tests covering:
 *	    T1   identity: enabled=false -> LEGACY for any node_id
 *	    T2   identity: enabled + node_id=-1 -> LEGACY (unset stays legacy)
 *	    T3   identity: node 0 -> thread 1; node 127 -> thread 128 (=MAX)
 *	    T4   dir_name endpoints: "thread_1" / "thread_128"; never "thread_0"
 *	    T5   validator: cluster_flags != 0 rejected for any thread_id
 *	    T6   validator: LEGACY accepted under any expected value
 *	    T7   validator: real id accepted under expected=INVALID (any) and
 *	         under expected=same; rejected under expected=other (RL1 matrix)
 *	    T8   validator: out-of-range rejected (MAX+1, MAX_REAL, 0xFFFF)
 *	    T9   claim fill/validate round-trip (all identity fields + crc)
 *	    T10  claim corruption rejected: crc flip / magic / version
 *	    T11  claim identity mismatch rejected: thread_id / node_id
 *	    T12  Stage 1 strict wrapper semantics unchanged (0/0 only)
 *
 *	  Linkage mirrors test_cluster_undo_record: header-only inclusion +
 *	  libpgcommon/libpgport for pg_crc32c -- no module .o, no stubs.
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-4.1-per-thread-wal-routing.md (FROZEN v1.0)
 *
 *-------------------------------------------------------------------------
 */
#define USE_PGRAC_CLUSTER 1

#include "postgres.h"

#include <stddef.h>
#include <string.h>

#include "cluster/cluster_wal_thread.h"

#undef printf
#undef fprintf
#undef snprintf

#include "unit_test.h"

UT_DEFINE_GLOBALS();


/*
 * Assert backstop: cassert builds pull libpgport_srv objects (snprintf.c
 * via the header's dir-name builder) that reference ExceptionalCondition.
 */
void
ExceptionalCondition(const char *conditionName pg_attribute_unused(),
					 const char *fileName pg_attribute_unused(),
					 int lineNumber pg_attribute_unused())
{
	abort();
}


/* ---- T1: identity is LEGACY whenever the cluster is disabled ---- */
UT_TEST(test_identity_disabled_is_legacy)
{
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(false, -1), (int)XLP_THREAD_ID_LEGACY);
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(false, 0), (int)XLP_THREAD_ID_LEGACY);
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(false, 127), (int)XLP_THREAD_ID_LEGACY);
}

/* ---- T2: enabled but node_id unset stays LEGACY ---- */
UT_TEST(test_identity_unset_node_is_legacy)
{
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(true, -1), (int)XLP_THREAD_ID_LEGACY);
}

/* ---- T3: node 0 -> thread 1; node 127 -> thread 128 == MAX ---- */
UT_TEST(test_identity_real_mapping)
{
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(true, 0), (int)XLP_THREAD_ID_FIRST_REAL);
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(true, 127), (int)CLUSTER_WAL_THREAD_MAX);
	UT_ASSERT_EQ((int)cluster_wal_thread_id_for(true, 5), 6);
}

/* ---- T4: directory-name endpoints; the sentinel is not a directory ---- */
UT_TEST(test_dir_name_endpoints)
{
	char buf[64];

	cluster_wal_thread_dir_name(XLP_THREAD_ID_FIRST_REAL, buf, sizeof(buf));
	UT_ASSERT_EQ(strcmp(buf, "thread_1"), 0);

	cluster_wal_thread_dir_name(CLUSTER_WAL_THREAD_MAX, buf, sizeof(buf));
	UT_ASSERT_EQ(strcmp(buf, "thread_128"), 0);

	/* No real id maps to "thread_0" (namespace matrix, spec-4.1 §2.1). */
	cluster_wal_thread_dir_name(cluster_wal_thread_id_for(true, 0), buf, sizeof(buf));
	UT_ASSERT_EQ(strcmp(buf, "thread_0") != 0, 1);
}

/* ---- T5: non-zero cluster_flags is rejected for any thread_id ---- */
UT_TEST(test_validator_flags_rejected)
{
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(XLP_THREAD_ID_LEGACY, 1, XLP_THREAD_ID_INVALID),
				 false);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(2, 0x8000, XLP_THREAD_ID_INVALID), false);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(2, 1, 2), false);
}

/* ---- T6: LEGACY accepted under any expected value ---- */
UT_TEST(test_validator_legacy_always_ok)
{
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(XLP_THREAD_ID_LEGACY, XLP_CLUSTER_FLAGS_RESERVED,
												   XLP_THREAD_ID_INVALID),
				 true);
	/* mixed segments: own-stream strict recovery still accepts legacy pages */
	UT_ASSERT_EQ(
		cluster_xlog_validate_page_header(XLP_THREAD_ID_LEGACY, XLP_CLUSTER_FLAGS_RESERVED, 7),
		true);
}

/* ---- T7: RL1 matrix -- any-valid vs own-stream strict ---- */
UT_TEST(test_validator_rl1_matrix)
{
	/* tools / standby / cross-thread diagnostics: expected = INVALID = any */
	UT_ASSERT_EQ(
		cluster_xlog_validate_page_header(1, XLP_CLUSTER_FLAGS_RESERVED, XLP_THREAD_ID_INVALID),
		true);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(
					 CLUSTER_WAL_THREAD_MAX, XLP_CLUSTER_FLAGS_RESERVED, XLP_THREAD_ID_INVALID),
				 true);
	/* own-stream strict: same id passes, different id is rejected */
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(7, XLP_CLUSTER_FLAGS_RESERVED, 7), true);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(7, XLP_CLUSTER_FLAGS_RESERVED, 8), false);
}

/* ---- T8: out-of-range real ids rejected ---- */
UT_TEST(test_validator_range_rejected)
{
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(
					 CLUSTER_WAL_THREAD_MAX + 1, XLP_CLUSTER_FLAGS_RESERVED, XLP_THREAD_ID_INVALID),
				 false);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(
					 XLP_THREAD_ID_MAX_REAL, XLP_CLUSTER_FLAGS_RESERVED, XLP_THREAD_ID_INVALID),
				 false);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header(
					 XLP_THREAD_ID_INVALID, XLP_CLUSTER_FLAGS_RESERVED, XLP_THREAD_ID_INVALID),
				 false);
}

/* ---- T9: claim fill/validate round-trip ---- */
UT_TEST(test_claim_roundtrip)
{
	ClusterWalThreadClaim claim;
	const char *reason = (const char *)0x1;

	UT_ASSERT_EQ((int)sizeof(ClusterWalThreadClaim), 40);

	cluster_wal_thread_claim_fill(&claim, 3, 2, 1234567890LL);
	UT_ASSERT_EQ(claim.magic == CLUSTER_WAL_THREAD_CLAIM_MAGIC, true);
	UT_ASSERT_EQ((int)claim.version, (int)CLUSTER_WAL_THREAD_CLAIM_VERSION);
	UT_ASSERT_EQ(cluster_wal_thread_claim_validate(&claim, 3, 2, &reason), true);
	UT_ASSERT_EQ(reason == NULL, true);
}

/* ---- T10: corruption rejected (crc / magic / version) ---- */
UT_TEST(test_claim_corruption_rejected)
{
	ClusterWalThreadClaim claim;
	const char *reason = NULL;

	cluster_wal_thread_claim_fill(&claim, 3, 2, 42);
	claim.created_at ^= 1; /* body flip without recomputing crc */
	UT_ASSERT_EQ(cluster_wal_thread_claim_validate(&claim, 3, 2, &reason), false);
	UT_ASSERT_EQ(strcmp(reason, "bad crc"), 0);

	cluster_wal_thread_claim_fill(&claim, 3, 2, 42);
	claim.magic = 0xDEADBEEF;
	UT_ASSERT_EQ(cluster_wal_thread_claim_validate(&claim, 3, 2, &reason), false);
	UT_ASSERT_EQ(strcmp(reason, "bad magic"), 0);

	cluster_wal_thread_claim_fill(&claim, 3, 2, 42);
	claim.version = 99;
	UT_ASSERT_EQ(cluster_wal_thread_claim_validate(&claim, 3, 2, &reason), false);
	UT_ASSERT_EQ(strcmp(reason, "bad version"), 0);
}

/* ---- T11: identity mismatch rejected (foreign claim) ---- */
UT_TEST(test_claim_identity_mismatch_rejected)
{
	ClusterWalThreadClaim claim;
	const char *reason = NULL;

	cluster_wal_thread_claim_fill(&claim, 3, 2, 42);
	UT_ASSERT_EQ(cluster_wal_thread_claim_validate(&claim, 4, 2, &reason), false);
	UT_ASSERT_EQ(strcmp(reason, "thread_id mismatch"), 0);

	cluster_wal_thread_claim_fill(&claim, 3, 2, 42);
	UT_ASSERT_EQ(cluster_wal_thread_claim_validate(&claim, 3, 9, &reason), false);
	UT_ASSERT_EQ(strcmp(reason, "node_id mismatch"), 0);
}

/* ---- T12: Stage 1 strict wrapper semantics unchanged ---- */
UT_TEST(test_stage1_wrapper_unchanged)
{
	UT_ASSERT_EQ(cluster_xlog_validate_page_header_stage1_invariant(0, 0), true);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header_stage1_invariant(1, 0), false);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header_stage1_invariant(0, 1), false);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header_thread_id(0), true);
	UT_ASSERT_EQ(cluster_xlog_validate_page_header_thread_id(3), false);
}


int
main(int argc, char **argv)
{
	UT_PLAN(12);

	UT_RUN(test_identity_disabled_is_legacy);
	UT_RUN(test_identity_unset_node_is_legacy);
	UT_RUN(test_identity_real_mapping);
	UT_RUN(test_dir_name_endpoints);
	UT_RUN(test_validator_flags_rejected);
	UT_RUN(test_validator_legacy_always_ok);
	UT_RUN(test_validator_rl1_matrix);
	UT_RUN(test_validator_range_rejected);
	UT_RUN(test_claim_roundtrip);
	UT_RUN(test_claim_corruption_rejected);
	UT_RUN(test_claim_identity_mismatch_rejected);
	UT_RUN(test_stage1_wrapper_unchanged);

	UT_DONE();
	return ut_failed_count != 0 ? 1 : 0;
}
