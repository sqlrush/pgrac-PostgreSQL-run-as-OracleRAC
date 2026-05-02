/*-------------------------------------------------------------------------
 *
 * cluster_pcm_lock.h
 *	  pgrac cluster PCM (Parallel Cache Management) lock framework
 *	  scaffolding (Stage 1.7 stub).
 *
 *	  Stage 1.7 ships only the API typedefs + opaque GrdEntry forward
 *	  declaration + 6 stub function prototypes + 1 GUC + shmem region
 *	  registration (size 0 by default).  All 4 mutation API calls
 *	  (acquire / release / upgrade / downgrade) ereport
 *	  ERRCODE_FEATURE_NOT_SUPPORTED with errmsg "PCM lock manager is
 *	  not implemented in Stage 1.7 stub".  Stage 2.X (#15 PCM state
 *	  machine + #11/#12 GRD master + #17 Cache Fusion) replaces the
 *	  4 stub function bodies with the actual 9-transition state
 *	  machine + Convert Queue + master routing protocol.
 *
 *	  GrdEntry is intentionally an opaque struct (Q3 user 修订
 *	  2026-05-02): the full struct definition lives in
 *	  src/backend/cluster/cluster_pcm_lock.c (private).  Header
 *	  exposes only the typedef + 3 helper functions
 *	  (cluster_pcm_grd_count / cluster_pcm_grd_shmem_size /
 *	  cluster_pcm_grd_init).  Stage 2.X spec adds bitmap (s_holders /
 *	  pi_holders) and convert_queue fields without breaking ABI of
 *	  Stage 1.7 callers (none yet, but pattern locked).  Matches
 *	  PG's `typedef struct CheckpointStatsData CheckpointStatsData;`
 *	  opaque idiom in xlog.h.
 *
 *	  GUC cluster.pcm_grd_max_entries default 0 (Q4 user 修订): the
 *	  cluster_pcm_grd shmem region is registered but allocated 0
 *	  bytes by default; users may set non-zero to verify shmem
 *	  pre-allocation startup stability (1.7 stub still ereports).
 *	  Stage 2.X PCM 真值激活 spec changes default to NBuffers.
 *
 *	  Why Stage 1.7 frame this scaffolding:
 *	  - PG-side has no PCM concept; cluster needs a clearly-named
 *	    layer with C internal API + shmem framework + diagnostic
 *	    surface so Stage 2 truth-activation just replaces stub
 *	    function bodies (no ABI break for callers).
 *	  - Inject points (4 of them, registered at stage 1.7) lock the
 *	    state-machine transition trace points for Stage 2 fault
 *	    injection testing without re-shuffling baseline numbers.
 *	  - pg_cluster_state.pcm category surfaces 6 keys for DBA
 *	    diagnostics (max_entries / allocated_bytes / active_entries
 *	    / mode_count / transition_count / api_state="stub").
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_pcm_lock.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-1.7-pcm-state-placeholder.md (frozen 2026-05-02 v1.1)
 *	  Design: docs/pcm-lock-protocol-design.md v1.0 §3-§5
 *	  AD-002 (PCM lock state machine N/S/X + PI orthogonal flag)
 *	  + AD-005 (Cache Fusion full; cf_state stub via BufferDesc)
 *	  + AD-006 (CR construction; PCM_TRANS_S_TO_X_CLEANOUT placeholder)
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_PCM_LOCK_H
#define CLUSTER_PCM_LOCK_H

#include "c.h"
#include "cluster/cluster_buffer_desc.h" /* PcmState (1.6), INVALID_NODE_ID */
#include "storage/buf_internals.h"		 /* BufferTag */

#ifdef USE_PGRAC_CLUSTER

/*
 * PcmLockMode -- API-level alias of PcmState (cluster_buffer_desc.h).
 *
 *	BufferDesc field stays named pcm_state (1.6 introduced); PCM lock
 *	API parameters use PcmLockMode for namespace clarity.  Same enum,
 *	same values via typedef alias (avoids value drift).
 *
 *	Constant aliases PCM_LOCK_MODE_N/S/X let API callers write
 *	  cluster_pcm_lock_acquire(tag, PCM_LOCK_MODE_S)
 *	rather than
 *	  cluster_pcm_lock_acquire(tag, PCM_STATE_S)
 *	while sharing the underlying 0/1/2 values exactly.
 *
 *	Spec: spec-1.7 §1.4 example #4 (Q2 user 修订 2026-05-02 verified).
 */
typedef PcmState PcmLockMode;
#define PCM_LOCK_MODE_N PCM_STATE_N
#define PCM_LOCK_MODE_S PCM_STATE_S
#define PCM_LOCK_MODE_X PCM_STATE_X


/*
 * PcmLockTransition -- 9 legal state-machine transitions.
 *
 *	Defined per docs/pcm-lock-protocol-design.md §4.1 + AD-002.
 *	Stage 1.7 ships only the enum values (used in pg_cluster_state.pcm
 *	transition_count = 9 and as future Stage 2.X state-machine input).
 *	Actual transition logic is empty in Stage 1.7 (stub functions
 *	ereport before any transition).
 *
 *	Transition #9 (S→X cleanout) is reader-triggered ITL cleanout per
 *	AD-006 第四轮; Stage 3 (AD-006 第五轮 ~27000 LOC) wires actual
 *	cleanout calls.
 */
typedef enum PcmLockTransition {
	PCM_TRANS_N_TO_S = 1,			 /* read-first */
	PCM_TRANS_N_TO_X = 2,			 /* write-first */
	PCM_TRANS_S_TO_X_UPGRADE = 3,	 /* self upgrade */
	PCM_TRANS_X_TO_S_DOWNGRADE = 4,	 /* downgrade with PI */
	PCM_TRANS_X_TO_N_DOWNGRADE = 5,	 /* full downgrade with PI */
	PCM_TRANS_X_TO_N_RELEASE = 6,	 /* release without PI */
	PCM_TRANS_S_TO_N_INVALIDATE = 7, /* invalidated by remote X request */
	PCM_TRANS_S_TO_N_RELEASE = 8,	 /* local release */
	PCM_TRANS_S_TO_X_CLEANOUT = 9	 /* AD-006 ITL cleanout */
} PcmLockTransition;
#define PCM_TRANSITION_COUNT 9


/*
 * GrdEntry -- opaque per-block global lock state (master node).
 *
 *	Stage 1.7 (Q3 user 修订 2026-05-02): full struct definition lives
 *	in src/backend/cluster/cluster_pcm_lock.c (private).  Header
 *	exposes only the typedef + 3 helper functions
 *	(cluster_pcm_grd_count / cluster_pcm_grd_shmem_size /
 *	cluster_pcm_grd_init).
 *
 *	Why opaque: Stage 2.X (#11/#12 GRD master + #15 PCM state machine)
 *	will add bitmap fields (s_holders / pi_holders) and a
 *	convert_queue field.  An opaque struct lets Stage 2 evolve
 *	internal layout without breaking the ABI of any (future) callers.
 *	Matches PG's `typedef struct CheckpointStatsData CheckpointStatsData;`
 *	idiom in xlog.h (implementation evolves; header interface stays).
 */
typedef struct GrdEntry GrdEntry;


/*
 * GUC cluster.pcm_grd_max_entries -- maximum number of GrdEntry slots
 *	in the cluster_pcm_grd shmem region.
 *
 *	Default 0 (Q4 user 修订): no GRD shmem allocated by default.
 *	cluster_pcm_grd_init handles size=0 by early-returning before
 *	ShmemInitStruct (Q5 user 修订: PG ShmemInitStruct(name, 0,
 *	&found) behavior is undefined).  Range [0, 1048576] (max ~128 MB
 *	at sizeof(GrdEntry) ~128 B).  PGC_POSTMASTER (startup-fixed).
 *
 *	Stage 2.X PCM 真值激活 spec changes default to NBuffers (one
 *	GrdEntry per shared buffer block, since master tracks all blocks).
 */
extern int cluster_pcm_grd_max_entries;


/*
 * PCM lock mutation API -- Stage 1.7 stubs.
 *
 *	All 4 functions ereport(ERROR, ERRCODE_FEATURE_NOT_SUPPORTED,
 *	errmsg("PCM lock manager is not implemented in Stage 1.7 stub"),
 *	errhint("Stage 1.7 exposes API and shmem scaffolding only; PCM
 *	lock transitions land in Stage 2.")) -- Q1 user 修订 2026-05-02
 *	精确 errmsg/errhint 文本.
 *
 *	Stage 1.7 has NO SQL-callable function (Q8 user 修订 2026-05-02
 *	strong condition): cluster_pcm_lock_* are C internal only;
 *	pg_proc.dat is not modified, system_views.sql is not modified,
 *	no new catalog tuples / system view columns / SQL function
 *	binding are added.  This keeps catversion = 202605050 (no bump).
 *	Stage 2.X PCM 真值激活 spec decides if any SQL binding is added.
 *
 *	Inject points wrap each function entry (Q6 user 修订 2026-05-02
 *	release-pre instead of release-post for naming honesty -- 1.7
 *	stub never reaches a 'post' point because it ereports immediately):
 *	  cluster_pcm_lock_acquire   -> "cluster-pcm-acquire-entry"
 *	  cluster_pcm_lock_release   -> "cluster-pcm-release-pre"
 *	  cluster_pcm_lock_upgrade   -> "cluster-pcm-convert-pre"
 *	  cluster_pcm_lock_downgrade -> "cluster-pcm-downgrade-pre"
 */
extern void cluster_pcm_lock_acquire(BufferTag tag, PcmLockMode mode);
extern void cluster_pcm_lock_release(BufferTag tag);
extern void cluster_pcm_lock_upgrade(BufferTag tag);
extern void cluster_pcm_lock_downgrade(BufferTag tag, PcmLockMode target_mode, bool keep_pi);


/*
 * Diagnostic / introspection helpers (always-callable).
 *
 *	cluster_pcm_lock_query: returns PCM_LOCK_MODE_N at stage 1.7
 *	  (no PCM lock held anywhere; consistent with BufferDesc.pcm_state
 *	  placeholder in 1.6).
 *
 *	cluster_pcm_grd_count: returns 0 at stage 1.7 (no entries
 *	  populated regardless of GUC value; stub never writes to GRD).
 *
 *	cluster_pcm_grd_shmem_size: returns 0 if GUC=0 (default), else
 *	  cluster_pcm_grd_max_entries * sizeof(struct GrdEntry).  Used
 *	  by the spec-1.3 shmem registry size_fn callback.
 *
 *	cluster_pcm_grd_init: shmem registry init_fn callback.  Q5 user
 *	  修订: early-returns if cluster_pcm_grd_max_entries == 0
 *	  (avoids ShmemInitStruct undefined-behavior at size=0); else
 *	  allocates + zero-init's + LWLockInitialize per entry.
 */
extern PcmLockMode cluster_pcm_lock_query(BufferTag tag);
extern int cluster_pcm_grd_count(void);
extern Size cluster_pcm_grd_shmem_size(void);
extern void cluster_pcm_grd_init(void);


/*
 * Module-level shmem registration entry point.
 *
 *	Called from cluster_init_shmem_module() (cluster_shmem.c) to
 *	register the cluster_pcm_grd region with the spec-1.3 shmem
 *	registry.  Idempotent (registry checks for duplicate names).
 *
 *	Stage 1.7: registers the region with size_fn = cluster_pcm_grd_
 *	shmem_size / init_fn = cluster_pcm_grd_init / lwlock_count = 0
 *	(per-entry LWLockInitialize happens in init_fn) /
 *	owner_subsys = "cluster_pcm".
 */
extern void cluster_pcm_lock_module_init(void);


#endif /* USE_PGRAC_CLUSTER */
#endif /* CLUSTER_PCM_LOCK_H */
