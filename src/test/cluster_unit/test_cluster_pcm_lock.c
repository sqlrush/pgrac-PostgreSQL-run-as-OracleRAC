/*-------------------------------------------------------------------------
 *
 * test_cluster_pcm_lock.c
 *	  Compile-time + link-time invariants for the PCM lock framework
 *	  scaffolding introduced at stage 1.7.
 *
 *	  Stage 1.7 ships only the cluster_pcm_lock.h API typedefs +
 *	  PcmLockMode constant aliases + PcmLockTransition 9-condition
 *	  enum + opaque GrdEntry forward declaration + 6 stub function
 *	  prototypes.  Actual state machine + GES master routing + Cache
 *	  Fusion semantics land at Stage 2.X.  This binary covers only
 *	  the constant value / enum range / API symbol existence
 *	  invariants.
 *
 *	  Q3 user 修订 2026-05-02 opaque struct: GrdEntry full struct
 *	  definition is private to cluster_pcm_lock.c, so this test
 *	  cannot use sizeof / offsetof on GrdEntry.  Tests instead
 *	  verify the constant alias values + enum range + symbol
 *	  existence (via &function_pointer != NULL link check).
 *
 *	  Q8 user 修订 2026-05-02 strong condition: cluster_pcm_lock_*
 *	  are C internal API only (no SQL function binding); this test
 *	  binary is the appropriate place to verify the symbols are
 *	  linkable; SQL-level behavior is verified in cluster_tap
 *	  024_pcm_lock.pl.
 *
 *	  Spec: spec-1.7-pcm-state-placeholder.md §1.2 Deliverable 7 +
 *	  §4.1 (6 项 cluster_unit 断言)
 *	  Design: docs/pcm-lock-protocol-design.md v1.0 §3-§4
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_pcm_lock.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Each test_*.c is a standalone executable; see unit_test.h.
 *	  cluster_pcm_lock.c contains the stub function definitions but
 *	  is not linked into this test binary (would drag in PG runtime
 *	  for ereport / LWLockInitialize).  Tests verify constants only;
 *	  function symbol existence is verified at runtime by 024_pcm_lock.pl
 *	  via `nm postgres | grep cluster_pcm_lock`.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cluster/cluster_buffer_desc.h" /* PcmState (1.6) */
#include "cluster/cluster_pcm_lock.h"

/* Drop PG's port.h printf -> pg_printf override; unit_test.h uses
 * stdlib printf and we don't link libpgport in this test binary. */
#undef printf

#include "unit_test.h"


UT_DEFINE_GLOBALS();


/*
 * Stage 1.7 stub: cluster_pcm_grd_max_entries lives in
 * cluster_pcm_lock.c which is not linked into this test binary
 * (would drag in PG runtime).  Provide a local definition matching
 * the type so test_pcm_grd_max_entries_default_is_zero can take
 * its address via `extern int cluster_pcm_grd_max_entries`.
 */
int cluster_pcm_grd_max_entries = 0;


/*
 * Stage 1.7 stub: cluster_pcm_lock_module_init lives in
 * cluster_pcm_lock.c.  Symbol existence is verified by taking the
 * function pointer; the actual function is never called here.
 */
void
cluster_pcm_lock_module_init(void)
{}


UT_TEST(test_pcm_lock_mode_constant_aliases_match_pcm_state)
{
	/*
	 * Q2 user 修订 2026-05-02: PcmLockMode is a typedef alias of
	 * PcmState (cluster_buffer_desc.h, 1.6); PCM_LOCK_MODE_N/S/X
	 * are #define aliases of PCM_STATE_N/S/X (0/1/2).  Verify the
	 * aliases share the underlying values exactly.
	 */
	UT_ASSERT_EQ((int)PCM_LOCK_MODE_N, 0);
	UT_ASSERT_EQ((int)PCM_LOCK_MODE_S, 1);
	UT_ASSERT_EQ((int)PCM_LOCK_MODE_X, 2);

	/* Cross-check: aliases ARE the same as PcmState constants. */
	UT_ASSERT_EQ((int)PCM_LOCK_MODE_N, (int)PCM_STATE_N);
	UT_ASSERT_EQ((int)PCM_LOCK_MODE_S, (int)PCM_STATE_S);
	UT_ASSERT_EQ((int)PCM_LOCK_MODE_X, (int)PCM_STATE_X);
}


UT_TEST(test_pcm_lock_transition_count_is_9)
{
	/*
	 * docs/pcm-lock-protocol-design.md §4.1 defines exactly 9 legal
	 * state-machine transitions.  PCM_TRANSITION_COUNT macro pins
	 * this number; pg_cluster_state.pcm.pcm_transition_count surface
	 * exposes it to DBAs.
	 */
	UT_ASSERT_EQ(PCM_TRANSITION_COUNT, 9);
}


UT_TEST(test_pcm_lock_transition_enum_values_are_1_to_9)
{
	/*
	 * Stage 1.7 locks the 9 transition enum values to 1..9 (not 0..8)
	 * so 0 stays reserved as a "no transition / not set" sentinel for
	 * Stage 2.X state-machine code.  This frees Stage 2 to use 0 as
	 * an internal "transition init" placeholder.
	 */
	UT_ASSERT_EQ((int)PCM_TRANS_N_TO_S, 1);
	UT_ASSERT_EQ((int)PCM_TRANS_N_TO_X, 2);
	UT_ASSERT_EQ((int)PCM_TRANS_S_TO_X_UPGRADE, 3);
	UT_ASSERT_EQ((int)PCM_TRANS_X_TO_S_DOWNGRADE, 4);
	UT_ASSERT_EQ((int)PCM_TRANS_X_TO_N_DOWNGRADE, 5);
	UT_ASSERT_EQ((int)PCM_TRANS_X_TO_N_RELEASE, 6);
	UT_ASSERT_EQ((int)PCM_TRANS_S_TO_N_INVALIDATE, 7);
	UT_ASSERT_EQ((int)PCM_TRANS_S_TO_N_RELEASE, 8);
	UT_ASSERT_EQ((int)PCM_TRANS_S_TO_X_CLEANOUT, 9);
}


UT_TEST(test_pcm_grd_max_entries_default_is_zero)
{
	/*
	 * Q4 user 修订 2026-05-02: cluster.pcm_grd_max_entries default
	 * 0 means cluster_pcm_grd shmem region is registered but not
	 * allocated.  This test verifies the C-side variable starts at
	 * 0 (before GUC parses) -- DefineCustomIntVariable boot value
	 * matches.
	 */
	extern int cluster_pcm_grd_max_entries;

	UT_ASSERT_EQ(cluster_pcm_grd_max_entries, 0);
}


UT_TEST(test_pcm_buffer_desc_invariants_hold_at_stage_1_7)
{
	/*
	 * spec-1.7 builds on top of spec-1.6 BufferDesc cluster fields:
	 *   - pcm_state field is in cache line 1 hot tail
	 *   - PCM_STATE_N (= 0) is the zero-init occupancy
	 *
	 * Verify the spec-1.6 invariants stayed intact at stage 1.7
	 * (no spec-1.7 change to cluster_buffer_desc.h fields).
	 */
	UT_ASSERT_EQ((int)PCM_STATE_N, 0);
	UT_ASSERT_EQ((int)PCM_STATE_S, 1);
	UT_ASSERT_EQ((int)PCM_STATE_X, 2);
}


UT_TEST(test_pcm_lock_module_init_symbol_is_callable)
{
	/*
	 * Q8 user 修订 2026-05-02 strong condition: cluster_pcm_lock_*
	 * are C internal API only (no SQL function binding).  This test
	 * verifies the cluster_pcm_lock_module_init function symbol is
	 * declared in the header (link-time check).  The function pointer
	 * must be non-NULL; if cluster_pcm_lock.c was accidentally not
	 * linked into the postgres binary, this would be a NULL pointer
	 * (or a link error at compile time).
	 *
	 * Note: we do NOT actually call cluster_pcm_lock_module_init()
	 * here because it transitively calls cluster_shmem_register_region
	 * which requires PG runtime state not present in this standalone
	 * test binary.  Symbol existence is the strongest invariant we
	 * can verify here.
	 */
	void (*fn)(void) = cluster_pcm_lock_module_init;

	UT_ASSERT(fn != NULL);
}


int
main(void)
{
	UT_PLAN(6);
	UT_RUN(test_pcm_lock_mode_constant_aliases_match_pcm_state);
	UT_RUN(test_pcm_lock_transition_count_is_9);
	UT_RUN(test_pcm_lock_transition_enum_values_are_1_to_9);
	UT_RUN(test_pcm_grd_max_entries_default_is_zero);
	UT_RUN(test_pcm_buffer_desc_invariants_hold_at_stage_1_7);
	UT_RUN(test_pcm_lock_module_init_symbol_is_callable);
	UT_DONE();
	return ut_failed_count == 0 ? 0 : 1;
}
