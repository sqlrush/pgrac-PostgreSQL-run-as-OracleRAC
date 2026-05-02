/*-------------------------------------------------------------------------
 *
 * cluster_pcm_lock.c
 *	  pgrac cluster PCM (Parallel Cache Management) lock framework
 *	  scaffolding (Stage 1.7 stub implementation).
 *
 *	  All public API functions in this file are Stage 1.7 stubs that
 *	  ereport ERRCODE_FEATURE_NOT_SUPPORTED.  Stage 2.X (#15 PCM
 *	  state machine + #11/#12 GRD master + #17 Cache Fusion) replaces
 *	  the 4 mutation function bodies with the actual 9-transition
 *	  state machine + Convert Queue + master routing protocol.
 *
 *	  The full GrdEntry struct definition lives in this file (private)
 *	  per Q3 user 修订 2026-05-02 opaque struct decision.  Stage 2.X
 *	  spec adds bitmap (s_holders / pi_holders) and convert_queue
 *	  fields without breaking the ABI of any (future) callers.
 *
 *	  Stage 1.7 has NO SQL-callable function (Q8 user 修订 2026-05-02
 *	  strong condition): cluster_pcm_lock_* are C internal only;
 *	  pg_proc.dat is not modified, system_views.sql is not modified,
 *	  no new catalog tuples / system view columns / SQL function
 *	  binding are added.  This keeps catversion = 202605050 (no bump).
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_pcm_lock.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-1.7-pcm-state-placeholder.md (frozen 2026-05-02 v1.1)
 *	  Design: docs/pcm-lock-protocol-design.md v1.0 §3-§5
 *	  AD-002 (PCM lock state machine N/S/X + PI orthogonal flag)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_PGRAC_CLUSTER

#include "access/xlogdefs.h"
#include "cluster/cluster_inject.h"
#include "cluster/cluster_pcm_lock.h"
#include "cluster/cluster_scn.h"
#include "cluster/cluster_shmem.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/elog.h"


/*
 * GUC: cluster.pcm_grd_max_entries
 *
 *	Default 0 (Q4 user 修订): no GRD shmem allocated by default.
 *	Range [0, 1048576] (max ~128 MB at sizeof(GrdEntry) ~128 B).
 *	PGC_POSTMASTER (startup-fixed; must restart to change).
 *
 *	Stage 2.X PCM 真值激活 spec changes default to NBuffers.
 */
int cluster_pcm_grd_max_entries = 0;


/*
 * GrdEntry -- per-block global lock state (master node).
 *
 *	Private struct definition (Q3 user 修订 2026-05-02 opaque struct).
 *	Stage 1.7 ships only the count fields + tag + SCN/LSN + per-entry
 *	protect lock; Stage 2.X spec adds bitmap (s_holders / pi_holders)
 *	and convert_queue fields here.
 */
struct GrdEntry {
	BufferTag tag;			/* block identity */
	uint16 x_holder;		/* INVALID_NODE_ID at stage 1.7 */
	uint16 s_holder_count;	/* 0 at stage 1.7 (bitmap deferred to Stage 2) */
	uint16 pi_holder_count; /* 0 at stage 1.7 */
	uint16 reserved_1;		/* alignment padding */
	SCN latest_block_scn;	/* InvalidScn */
	XLogRecPtr latest_lsn;	/* InvalidXLogRecPtr */
	LWLock protect;			/* per-entry; never acquired at stage 1.7 */
							/*
	 * Stage 2.X future fields:
	 *   NodeIdSet s_holders;  -- 16-bit bitmap (16 nodes max per AD-012)
	 *   NodeIdSet pi_holders; -- 16-bit bitmap
	 *   ConvertQueue convert_queue;  -- LWLock-protected linked list
	 */
};


/* Module-level shmem pointer to the GrdEntry array (set in init_fn). */
static struct GrdEntry *GrdEntries = NULL;


/* ============================================================
 * Stub mutation API -- 4 ereport functions (Q1 精确 errmsg 文本).
 *
 *	All 4 stubs share identical errmsg / errhint per Q1 user 修订
 *	2026-05-02 to avoid per-function variation.  Inject points are
 *	wired at function entry; arm a fault to override the default
 *	ereport(0A000) main path with the framework-injected behavior.
 * ============================================================ */

#define PCM_STUB_NOT_IMPLEMENTED                                                                   \
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),                                        \
					errmsg("PCM lock manager is not implemented in Stage 1.7 stub"),               \
					errhint("Stage 1.7 exposes API and shmem scaffolding only; "                   \
							"PCM lock transitions land in Stage 2.")))


void
cluster_pcm_lock_acquire(BufferTag tag, PcmLockMode mode)
{
	(void)tag;
	(void)mode;
	CLUSTER_INJECTION_POINT("cluster-pcm-acquire-entry");
	PCM_STUB_NOT_IMPLEMENTED;
}


void
cluster_pcm_lock_release(BufferTag tag)
{
	/*
	 * release-PRE not release-POST per Q6 user 修订 2026-05-02:
	 * stage 1.7 stub never reaches a 'post' point because it ereports
	 * immediately; naming the inject point 'release-pre' is honest.
	 * Stage 2.X真值激活 keeps the inject point at release entry which
	 * remains semantically correct (still pre-state-machine work).
	 */
	(void)tag;
	CLUSTER_INJECTION_POINT("cluster-pcm-release-pre");
	PCM_STUB_NOT_IMPLEMENTED;
}


void
cluster_pcm_lock_upgrade(BufferTag tag)
{
	(void)tag;
	CLUSTER_INJECTION_POINT("cluster-pcm-convert-pre");
	PCM_STUB_NOT_IMPLEMENTED;
}


void
cluster_pcm_lock_downgrade(BufferTag tag, PcmLockMode target_mode, bool keep_pi)
{
	(void)tag;
	(void)target_mode;
	(void)keep_pi;
	CLUSTER_INJECTION_POINT("cluster-pcm-downgrade-pre");
	PCM_STUB_NOT_IMPLEMENTED;
}


/* ============================================================
 * Diagnostic / introspection helpers (always-callable).
 * ============================================================ */

PcmLockMode
cluster_pcm_lock_query(BufferTag tag)
{
	(void)tag;
	/* Stage 1.7: no PCM lock held anywhere; always return N. */
	return PCM_LOCK_MODE_N;
}


int
cluster_pcm_grd_count(void)
{
	/* Stage 1.7: no entries populated regardless of GUC value. */
	return 0;
}


Size
cluster_pcm_grd_shmem_size(void)
{
	if (cluster_pcm_grd_max_entries == 0)
		return 0;
	return mul_size(cluster_pcm_grd_max_entries, sizeof(struct GrdEntry));
}


void
cluster_pcm_grd_init(void)
{
	bool found;

	/*
	 * Q5 user 修订 2026-05-02: size=0 path safe early-return.
	 *
	 * PG ShmemInitStruct(name, 0, &found) behavior is undefined.
	 * Default GUC=0 main path returns here without touching shmem;
	 * pg_cluster_shmem view shows region_size_bytes = 0 (registered
	 * but not allocated).
	 */
	if (cluster_pcm_grd_max_entries == 0)
		return;

	GrdEntries = (struct GrdEntry *)ShmemInitStruct(
		"pgrac cluster pcm grd", mul_size(cluster_pcm_grd_max_entries, sizeof(struct GrdEntry)),
		&found);

	if (!found) {
		/* Zero-init + per-entry LWLockInitialize. */
		memset(GrdEntries, 0, mul_size(cluster_pcm_grd_max_entries, sizeof(struct GrdEntry)));
		for (int i = 0; i < cluster_pcm_grd_max_entries; i++) {
			/*
			 * Stage 1.7 reuses LWTRANCHE_BUFFER_PCM_LOCK (1.6.1 hardening
			 * registered tranche).  pcm_lock per BufferDesc + GrdEntry.protect
			 * share the same tranche to keep lock-trace output simple
			 * ("BufferPcmLock" name shows for both).
			 */
			LWLockInitialize(&GrdEntries[i].protect, LWTRANCHE_BUFFER_PCM_LOCK);
		}
	}
}


/* ============================================================
 * Module-level shmem registration.
 * ============================================================ */

static const ClusterShmemRegion cluster_pcm_grd_region = {
	.name = "pgrac cluster pcm grd",
	.size_fn = cluster_pcm_grd_shmem_size,
	.init_fn = cluster_pcm_grd_init,
	.lwlock_count = 0, /* per-entry LWLock initialized in init_fn */
	.owner_subsys = "cluster_pcm",
	.reserved_flags = 0,
};


void
cluster_pcm_lock_module_init(void)
{
	/*
	 * Register cluster_pcm_grd region with the spec-1.3 shmem registry.
	 *
	 * Idempotent (registry checks for duplicate names); safe to call
	 * from cluster_init_shmem_module() once per postmaster start.
	 */
	cluster_shmem_register_region(&cluster_pcm_grd_region);
}


#endif /* USE_PGRAC_CLUSTER */
