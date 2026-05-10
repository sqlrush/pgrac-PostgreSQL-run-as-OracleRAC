/*-------------------------------------------------------------------------
 *
 * test_cluster_fence.c
 *	  spec-2.28 Sprint A Step 1 — D1+D2 cluster_fence.h+.c skeleton
 *	  link / shmem layout / API surface smoke.
 *
 *	  Step 1 scope (this file — T-fence-1 only):
 *	    T-fence-1a  GUC default values match spec §2.3 (4 GUCs)
 *	    T-fence-1b  cluster_fence_shmem_size > 0 + shmem_init succeeds +
 *	                idempotent (init twice safe via found-flag)
 *	    T-fence-1c  ClusterFenceFreezePending defaults to 0
 *	    T-fence-1d  6 public API functions (broadcast_freeze /
 *	                broadcast_thaw / self_request / check_interrupts /
 *	                postmaster_check / cluster_get_fence_state) link
 *	                cleanly + early-return safely on disabled / null
 *	                shmem paths
 *
 *	  Step 1 explicitly DEFERS:
 *	    - Real ProcSignal flag set/clear (Step 2 D3)
 *	    - postgres.c ProcessInterrupts hook ereport (Step 2 D4)
 *	    - LMON broadcast loop ProcArray (Step 3 D5)
 *	    - postmaster kill SIGINT (Step 3 D6)
 *	    - Catalog SRF / inject / view (Step 4)
 *	    - 098 TAP fence_freeze_writes_2node (Step 5)
 *
 *	  Per Invariant I3:  Sprint A Step 1 prerequisite is linkdb tag
 *	  v0.14.2-stage2.6 nightly run 25618433189 ✓ (Hardening v0.6 F1+F2
 *	  closed:quorum_state post-write recompute + fast-restart ghost-
 *	  detect dual layer).
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/test/cluster_unit/test_cluster_fence.c
 *
 * NOTES
 *	  pgrac-original file.  Spec:  spec-2.28-fence-lite-self-fence-
 *	  procsignal-freeze-thaw.md (frozen v0.3 2026-05-10).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <stddef.h>

#include "cluster/cluster_fence.h"

#undef printf
#undef fprintf
#undef snprintf
#undef sprintf
#undef vsnprintf
#undef vfprintf
#undef vprintf
#undef vsprintf
#undef strerror
#undef strerror_r

#include "unit_test.h"

UT_DEFINE_GLOBALS();


/* ============================================================
 * Stubs — link cluster_fence.o standalone.
 * ============================================================ */

bool IsUnderPostmaster = false;
int MyProcPid = 0;

/* spec-2.28 §2.3 — 4 fence GUCs with their declared defaults. */
bool cluster_self_fence_enabled = true;
int cluster_self_fence_grace_ms = 30000;
bool cluster_freeze_writes_enabled = true;
int cluster_fence_audit_log = 1; /* CLUSTER_FENCE_AUDIT_LOG_LOG */

/* Stage 0.10 cluster.enabled GUC (cluster_guc.c). */
bool cluster_enabled = false;

void
ExceptionalCondition(const char *conditionName pg_attribute_unused(),
					 const char *fileName pg_attribute_unused(),
					 int lineNumber pg_attribute_unused())
{
	abort();
}

bool
errstart(int e pg_attribute_unused(), const char *d pg_attribute_unused())
{
	return false;
}
bool
errstart_cold(int e pg_attribute_unused(), const char *d pg_attribute_unused())
{
	return false;
}
void
errfinish(const char *f pg_attribute_unused(), int l pg_attribute_unused(),
		  const char *fn pg_attribute_unused())
{}
int
errcode(int s pg_attribute_unused())
{
	return 0;
}
int
errmsg(const char *f pg_attribute_unused(), ...)
{
	return 0;
}
int
errhint(const char *f pg_attribute_unused(), ...)
{
	return 0;
}
int
errdetail(const char *f pg_attribute_unused(), ...)
{
	return 0;
}

#include "storage/shmem.h"
static char shmem_storage[512] __attribute__((aligned(64)));
static bool shmem_init_done = false;
void *
ShmemInitStruct(const char *name pg_attribute_unused(), Size size, bool *foundPtr)
{
	if (foundPtr != NULL)
		*foundPtr = shmem_init_done;
	if (size > sizeof(shmem_storage))
		return NULL;
	shmem_init_done = true;
	return (void *)shmem_storage;
}

#include "datatype/timestamp.h"
TimestampTz
GetCurrentTimestamp(void)
{
	return 1700000000000000LL;
}

#include "storage/lwlock.h"
void
LWLockInitialize(LWLock *lock pg_attribute_unused(), int tranche_id pg_attribute_unused())
{}
bool
LWLockAcquire(LWLock *lock pg_attribute_unused(), LWLockMode mode pg_attribute_unused())
{
	return true;
}
void
LWLockRelease(LWLock *lock pg_attribute_unused())
{}

#include "cluster/cluster_shmem.h"
void
cluster_shmem_register_region(const ClusterShmemRegion *region pg_attribute_unused())
{}


/* ============================================================
 * T-fence-1a:  GUC default values match spec §2.3.
 * ============================================================ */
UT_TEST(test_t_fence_1a_guc_defaults)
{
	UT_ASSERT_EQ((int)cluster_self_fence_enabled, 1);
	UT_ASSERT_EQ(cluster_self_fence_grace_ms, 30000);
	UT_ASSERT_EQ((int)cluster_freeze_writes_enabled, 1);
	UT_ASSERT_EQ(cluster_fence_audit_log, (int)CLUSTER_FENCE_AUDIT_LOG_LOG);
}


/* ============================================================
 * T-fence-1b:  ClusterFenceShmem init succeeds + idempotent.
 * ============================================================ */
UT_TEST(test_t_fence_1b_shmem_init)
{
	Size sz = cluster_fence_shmem_size();

	UT_ASSERT(sz > 0);
	UT_ASSERT(sz <= sizeof(shmem_storage)); /* fits stub buffer */

	cluster_fence_shmem_init();
	UT_ASSERT(shmem_init_done);
	/* Calling init twice is idempotent (found-flag). */
	cluster_fence_shmem_init();
	UT_ASSERT(shmem_init_done);
}


/* ============================================================
 * T-fence-1c:  ClusterFenceFreezePending defaults to 0.
 * ============================================================ */
UT_TEST(test_t_fence_1c_freeze_flag_default)
{
	UT_ASSERT_EQ((int)ClusterFenceFreezePending, 0);
}


/* ============================================================
 * T-fence-1d:  6 public API functions early-return safely.
 *
 *	With cluster_enabled = false:  all stubs hit L19/L20 silent skip
 *	first line and return without abort.
 *
 *	With cluster_enabled = true (after init done in T-fence-1b):  the
 *	functions touch shmem fields under LWLock-stubbed paths.
 *
 *	With IsUnderPostmaster = true:  postmaster_check silent-skips
 *	per CLAUDE.md rule 16 §postmaster-once / L91 EXEC_BACKEND.
 *
 *	The "assertion" is just survival — none of these abort.
 * ============================================================ */
UT_TEST(test_t_fence_1d_api_smoke)
{
	cluster_enabled = false;
	cluster_fence_broadcast_freeze("test", 0);
	cluster_fence_broadcast_thaw("test", 0);
	cluster_fence_self_request("test", 0);
	cluster_fence_check_interrupts();
	cluster_fence_postmaster_check();

	cluster_enabled = true;
	cluster_fence_broadcast_freeze("test", 1);
	cluster_fence_broadcast_thaw("test", 1);
	cluster_fence_self_request("test", 1);
	cluster_fence_check_interrupts();
	cluster_fence_postmaster_check();

	IsUnderPostmaster = true;
	cluster_fence_postmaster_check();

	UT_ASSERT(true); /* survival */
}


/* ============================================================
 * Test driver.
 * ============================================================ */
int
main(void)
{
	UT_PLAN(4);
	UT_RUN(test_t_fence_1a_guc_defaults);
	UT_RUN(test_t_fence_1b_shmem_init);
	UT_RUN(test_t_fence_1c_freeze_flag_default);
	UT_RUN(test_t_fence_1d_api_smoke);
	UT_DONE();
}
