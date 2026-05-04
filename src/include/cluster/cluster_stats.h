/*-------------------------------------------------------------------------
 *
 * cluster_stats.h
 *	  pgrac Cluster Stats cluster background process — Stage 1.14.
 *
 *	  Stage 1.14 ships the fourth cluster background process spawned by
 *	  postmaster (LMON 1.11 / LCK 1.12 / DIAG 1.13 preceded).  Sprint A and Sprint B were
 *	  shipped together: lifecycle skeleton + GUC + SQLSTATE + inject
 *	  points + wait event + dump_cluster_stats view, all in tag v0.2.0-stage1.14.
 *	  1.13 立的 phase4_sequence 直接复用; 1.14 是 phase 4 第二个 spawner further refreshes shmem on
 *	  every SPAWNING incarnation so SQL views never report stale PID /
 *	  timestamps after a ServerLoop respawn.
 *
 *	  The Cluster Stats main loop only does a local liveness tick
 *	  (last_liveness_tick_at advance + iter++) — it does NOT collect
 *	  fill pg_stat_cluster_* views, aggregate cross-node metrics,
 *	  track wait events, or retain history.  Those land in
 *	  Stage 2+ when interconnect is fully wired.
 *
 *	  Implemented surface (1.13 + 1.12.1 lessons preempted):
 *	    - cluster.cluster_stats_main_loop_interval GUC (PGC_SIGHUP, default 1000ms)
 *	    - SQLSTATE 53R10 STATS_SPAWN_FAILED / 53R11 STATS_NOT_READY
 *	    - 6 inject points cluster-stats-{pre-spawn,post-spawn,
 *	      ready-publish,main-loop-iter,shutdown-pre,shutdown-post}
 *	    - WAIT_EVENT_CLUSTER_BGPROC_CLUSTER_STATS_MAIN_LOOP wait event
 *	    - pg_cluster_state.cluster_stats view 7 keys (2 status + 5 lifecycle: status / status_enum_value /
 *	      pid / spawned_at / ready_at / last_liveness_tick_at /
 *	      main_loop_iters)
 *
 *	  HC1 (spec-1.11 §1.4): Cluster Stats spawn entry point Asserts
 *	      !IsUnderPostmaster (postmaster-only).  ClusterStatsMain itself
 *	      Asserts IsUnderPostmaster (reverse defense in depth).
 *
 *	  HC2 (spec-1.11 §1.4): ClusterStatsStatus enum is the single
 *	      source of truth.  All status writes go through the Cluster Stats
 *	      process itself (which holds the LWLock); postmaster only
 *	      writes shutdown_requested.
 *
 *	  HC3 (spec-1.11 §1.4): Sprint A boundary.  Real sample
 *	      sample collection / aggregation / view filling / history retention
 *	      data handling ALL deferred to Stage 2+ (interconnect
 *	      dependency).  Main loop is local liveness only.
 *
 *	  HC4 (spec-1.11 §1.4 + 4 实质 HC #2): phase_4_handler MUST have a
 *	      real reader of cluster_enabled GUC.  cluster_enabled=false →
 *	      phase_4_handler does NOT spawn Cluster Stats (degrades to spec-1.10
 *	      stub behavior).  Tested by 064 L10.
 *
 *	  HC5 (spec-1.11 §1.4 + 4 实质 HC #3): normal shutdown via
 *	      proc_exit(0) hits PG reaper's WIFEXITED + WEXITSTATUS=0
 *	      path -> no crash recovery.  Abnormal exit (signal /
 *	      non-zero) hits HandleChildCrash -> restart_after_crash
 *	      decides instance-level crash/restart.  PG existing reaper
 *	      auto-distinguishes; this file documents the contract.
 *
 *	  HC6 (spec-1.11 §1.4 + 4 实质 HC #4): Sprint A Cluster Stats does NOT
 *	      implement sample collection, aggregation, or view filling.
 *	      Field is named last_liveness_tick_at (NOT
 *	      last_sample_collected_at / last_aggregation_at) to avoid
 *	      conflating local main-loop tick with future stats sampling / aggregation
 *	      protocols (Stage 2+ cross-node aggregation).
 *
 *	  Q1 (spec-1.11 §1.4 Q-amend #1): 7-item AuxProcType integration —
 *	      enum / dispatch / start wrapper / EXEC_BACKEND argv /
 *	      BackendType + Am macro / ps display / reaper.  All wired in
 *	      Sprint A.
 *
 *	  Q2 (spec-1.11 §1.4 Q-amend #2): postmaster-owned narrow wrapper.
 *	      cluster_postmaster_start_stats() is implemented in
 *	      postmaster.c (so it can call file-static StartChildProcess);
 *	      cluster_stats_start() is a thin proxy declared here that
 *	      simply forwards to the postmaster-owned wrapper.  Call
 *	      chain: phase_4_handler → cluster_stats_start (thin) →
 *	      cluster_postmaster_start_stats (postmaster-owned) →
 *	      StartChildProcess (PG static).
 *
 *	  Q3 (spec-1.11 §1.4 Q-amend #3): readiness sync via bounded
 *	      polling (postmaster pre-ServerLoop has limited latch
 *	      infrastructure — no MyProc, shared latch ownership lifecycle
 *	      complex).  Postmaster polls shmem ready flag with
 *	      pg_usleep(100ms) loop bounded by cluster.phase4_timeout.
 *	      No latch in Sprint A; latch upgrade is reasonable Sprint B
 *	      consideration if needed.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_stats.h
 *
 * NOTES
 *	  This is a pgrac-original file.  Compiled only in --enable-cluster
 *	  builds.
 *	  Spec: spec-1.14-cluster-stats-skeleton.md (frozen 2026-05-04, Sprint A
 *	  scope).
 *	  Foundation: spec-1.10.1 ClusterPhaseSharedState shmem layout +
 *	  spec-1.10.2 cluster_phase mirror fix +
 *	  CLAUDE.md rule 16 §Postmaster-once.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_STATS_H
#define CLUSTER_STATS_H

#include "datatype/timestamp.h"
#include "storage/lwlock.h"


/*
 * ClusterStatsStatus -- HC2 SSOT for Cluster Stats lifecycle state.
 *
 *	The numeric values are observable via SQL (Sprint B view);
 *	preserve the existing 0..4 mapping when amending.
 */
typedef enum ClusterStatsStatus {
	CLUSTER_STATS_NOT_STARTED = 0, /* postmaster has not yet spawned Cluster Stats */
	CLUSTER_STATS_SPAWNING
	= 1, /* StartChildProcess returned a pid; Cluster Stats main not yet active */
	CLUSTER_STATS_READY = 2, /* Cluster Stats main loop active; phase 2 driver may advance */
	CLUSTER_STATS_SHUTTING_DOWN = 3, /* shutdown_requested set; Cluster Stats exiting */
	CLUSTER_STATS_EXITED = 4 /* Cluster Stats proc_exit complete; postmaster reaper to harvest */
} ClusterStatsStatus;

#define CLUSTER_STATS_STATUS_LAST CLUSTER_STATS_EXITED


/*
 * ClusterStatsSharedState -- Cluster Stats state visible across postmaster /
 * Cluster Stats / SQL backends.
 *
 *	Single writer for status / timestamps / iters: Cluster Stats process itself
 *	(takes lwlock LW_EXCLUSIVE).  Postmaster writes shutdown_requested
 *	(also LW_EXCLUSIVE).  Any backend reads (LW_SHARED) for SQL view.
 *
 *	Sprint A has no Latch field.  Sprint B may add one if the bounded-
 *	polling readiness path proves limiting.
 */
typedef struct ClusterStatsSharedState {
	LWLock lwlock;					   /* LWTRANCHE_CLUSTER_STATS guards everything below */
	ClusterStatsStatus status;		   /* HC2 SSOT */
	pid_t pid;						   /* set by Cluster Stats in CLUSTER_STATS_SPAWNING */
	TimestampTz spawned_at;			   /* set by Cluster Stats in CLUSTER_STATS_SPAWNING */
	TimestampTz ready_at;			   /* set by Cluster Stats in CLUSTER_STATS_READY */
	TimestampTz last_liveness_tick_at; /* HC6: local liveness tick — NOT inter-node heartbeat */
	int64 main_loop_iters;			   /* monotone counter; observable proof of liveness */
	bool shutdown_requested;		   /* postmaster sets; Cluster Stats main loop polls + exits */
} ClusterStatsSharedState;


/*
 * Public API.
 */

/*
 * Postmaster spawn helper (Q2 thin proxy).
 *
 *	Forwards to cluster_postmaster_start_stats() which lives in
 *	postmaster.c (so it can call the file-static StartChildProcess).
 *	Returns the Cluster Stats child pid on success, or 0 on spawn failure.
 *	Asserts !IsUnderPostmaster (HC1 defense in depth).
 */
extern int cluster_stats_start(void);

/*
 * Postmaster sync wait for Cluster Stats readiness (Q3 bounded polling).
 *
 *	Polls shmem state->status with pg_usleep(100ms) intervals up to
 *	timeout_ms.  Returns true if Cluster Stats reaches CLUSTER_STATS_READY in
 *	time, false on timeout.  Asserts !IsUnderPostmaster.
 */
extern bool cluster_stats_wait_for_ready(int timeout_ms);

/*
 * Postmaster shutdown signal (Q3 reverse path).
 *
 *	Sets state->shutdown_requested = true under LW_EXCLUSIVE; Cluster
 *	Stats main loop polls this flag every iteration and proc_exit(0)s
 *	cleanly when set.  Idempotent.  Asserts !IsUnderPostmaster.
 *
 *	Note (Q10): primary shutdown path is postmaster pmdie SIGTERM
 *	via signal_child(ClusterStatsPID, SIGTERM); this request_shutdown
 *	flag is belt-and-suspenders for non-SIGTERM shutdown sources.
 */
extern void cluster_stats_request_shutdown(void);

/*
 * Read-only accessors for SQL view + diagnostics.  LW_SHARED.
 *
 *	Spec-1.11.1 F11: Sprint B D12 only emitted lck_status +
 *	lck_status_enum_value, leaving cluster.cluster_stats_main_loop_interval
 *	GUC unverifiable from SQL (no main_loop_iters surface).  F11
 *	completes the 6-key view with the missing 5 accessors below.
 */
extern ClusterStatsStatus cluster_stats_status(void);
extern pid_t cluster_stats_pid(void);
extern TimestampTz cluster_stats_spawned_at(void);
extern TimestampTz cluster_stats_ready_at(void);
extern TimestampTz cluster_stats_last_liveness_tick_at(void);
extern int64 cluster_stats_main_loop_iters(void);

/*
 * Status enum -> canonical lowercase string ("not_started", "spawning",
 * "ready", "shutting_down", "exited").  Out-of-range returns
 * "(unknown)".
 */
extern const char *cluster_stats_status_to_string(ClusterStatsStatus s);

/*
 * shmem region helpers — registered by cluster_init_shmem_module()
 * via the spec-1.3 region registry.
 */
extern Size cluster_stats_shmem_size(void);
extern void cluster_stats_shmem_init(void);
extern void cluster_stats_shmem_register(void);

/*
 * AuxiliaryProcessMain dispatch entry.  HC1 reverse defense: Asserts
 * IsUnderPostmaster.  Never returns (proc_exit on shutdown).
 */
extern void ClusterStatsMain(void) pg_attribute_noreturn();


#endif /* CLUSTER_STATS_H */
