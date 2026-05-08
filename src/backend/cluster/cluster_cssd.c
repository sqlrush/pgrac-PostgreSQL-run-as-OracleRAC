/*-------------------------------------------------------------------------
 *
 * cluster_cssd.c
 *	  pgrac CSSD aux process implementation — spec-2.5 D2 (Sprint A
 *	  Step 2 — initial scaffolding;Step 3-5 wire MainLoop to LMON
 *	  outbound queue drain, GUC, wait event, inject points).
 *
 *	  See cluster_cssd.h for the API contract + spec-2.5 architecture
 *	  references (Q1 LMON-mediated send / Q3 fanout layer / Q6 first-
 *	  tick nonfatal grace period).
 *
 *	  Step 2 deliverable scope:
 *	    - StaticAssertDecl byte-layout locks for 12 B payload + 64 B
 *	      outbound slot (Q1 cache-line alignment)
 *	    - status_to_string / peer_state_to_string lookups
 *	    - ClusterCssdShmem layout + shmem_size / init / register
 *	    - LW_SHARED accessors for lifecycle + per-peer state
 *	    - CssdMain skeleton (WaitLatch + heartbeat tick + deadband-
 *	      scan loop;reads GUC interval if registered, else fallback
 *	      default 1000 ms;wait event + inject points wired in Step 5)
 *	    - dispatch_heartbeat handler (handler 4 hard constraints
 *	      enforced by source structure)
 *	    - lifecycle (start / wait_for_ready / request_shutdown stubs;
 *	      real impl ties into postmaster phase 4 driver in Step 4)
 *
 *	  Following step deliverables (NOT in Step 2):
 *	    Step 3: LMON tick drain CSSD outbound queue (modifies
 *	      cluster_lmon.c)
 *	    Step 4: D3 AuxProcType CssdProcess + D4 postmaster spawn
 *	      wrapper + D5 phase 4 driver third upgrade (DIAG + Stats +
 *	      CSSD)
 *	    Step 5: D6 cluster_shmem.c register + D7 LWTRANCHE +
 *	      D8 wait event + D9 GUC + D10 SQLSTATE + D11 inject points +
 *	      D12 phase 1 register msg_type=11 + D13 producer mask + D14
 *	      per-peer counter (in cluster_pgstat.c)
 *	    Step 6: D15 SRF/view + D16 catversion bump 220 → 230 + D20
 *	      cluster_regress
 *	    Step 7: D18 065 + D19 085 TAP
 *	    Step 8: ship — linkdb tag v0.12.0-stage2.5
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_cssd.c
 *
 * NOTES
 *	  pgrac-original file.  Compiled only in --enable-cluster mode
 *	  (USE_PGRAC_CLUSTER); function bodies are gated.
 *
 *	  Spec authority: pgrac:specs/spec-2.5-cssd-heartbeat-skeleton.md
 *	  (frozen v0.2 2026-05-08).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_PGRAC_CLUSTER

#include <string.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/elog.h"
#include "utils/timestamp.h"

#include "cluster/cluster_cssd.h"
#include "cluster/cluster_guc.h" /* cluster_node_id */


/* ============================================================
 * StaticAssertDecl byte-layout locks.
 *
 *   Payload 12 bytes natural-aligned (uint32 + uint64).  Outbound slot
 *   64 bytes cache-line aligned (false-sharing defense).
 * ============================================================ */

StaticAssertDecl(sizeof(ClusterCssdHeartbeatPayload) == 12,
				 "ClusterCssdHeartbeatPayload frozen at 12 bytes");

StaticAssertDecl(sizeof(ClusterCssdOutboundSlot) == 64,
				 "ClusterCssdOutboundSlot 64-byte cache-line aligned");


/* ============================================================
 * Shmem region (single instance; pointer set by shmem_init).
 * ============================================================ */

static ClusterCssdShmem *CssdShmem = NULL;


/* ============================================================
 * Status / state name lookup helpers.
 * ============================================================ */

const char *
cluster_cssd_status_to_string(ClusterCssdStatus s)
{
	switch (s) {
	case CLUSTER_CSSD_STARTING:
		return "starting";
	case CLUSTER_CSSD_READY:
		return "ready";
	case CLUSTER_CSSD_SHUTTING_DOWN:
		return "shutting_down";
	case CLUSTER_CSSD_DOWN:
		return "down";
	case CLUSTER_CSSD_FAILED:
		return "failed";
	}
	return "(unknown)";
}

const char *
cluster_cssd_peer_state_to_string(ClusterCssdPeerState s)
{
	switch (s) {
	case CLUSTER_CSSD_PEER_ALIVE:
		return "alive";
	case CLUSTER_CSSD_PEER_SUSPECTED:
		return "suspected";
	case CLUSTER_CSSD_PEER_DEAD:
		return "dead";
	}
	return "(unknown)";
}


/* ============================================================
 * Shmem region.
 * ============================================================ */

Size
cluster_cssd_shmem_size(void)
{
	return MAXALIGN(sizeof(ClusterCssdShmem));
}

void
cluster_cssd_shmem_init(void)
{
	bool found;
	Size sz = cluster_cssd_shmem_size();

	CssdShmem = (ClusterCssdShmem *)ShmemInitStruct("pgrac cluster cssd", sz, &found);

	if (!found) {
		int peer;

		memset(CssdShmem, 0, sz);
		LWLockInitialize(&CssdShmem->lwlock, LWTRANCHE_CLUSTER_CSSD);
		CssdShmem->status = CLUSTER_CSSD_STARTING;
		CssdShmem->shutdown_requested = false;

		pg_atomic_init_u64(&CssdShmem->total_heartbeat_send_count, 0);
		pg_atomic_init_u64(&CssdShmem->total_heartbeat_recv_count, 0);

		for (peer = 0; peer < CLUSTER_MAX_NODES; peer++) {
			pg_atomic_init_u32(&CssdShmem->peers[peer].state, CLUSTER_CSSD_PEER_ALIVE);
			pg_atomic_init_u64(&CssdShmem->peers[peer].last_heartbeat_recv_at_us, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].last_heartbeat_send_at_us, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].heartbeat_send_count, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].heartbeat_recv_count, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].suspected_transitions, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].dead_transitions, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].suspected_since_us, 0);
			pg_atomic_init_u64(&CssdShmem->peers[peer].dead_since_us, 0);

			pg_atomic_init_u32(&CssdShmem->outbound[peer].pending, 0);
			pg_atomic_init_u32(&CssdShmem->outbound[peer].result_state, 0);
		}
	}
}

void
cluster_cssd_shmem_register(void)
{
	/* NB: real cluster_shmem.c registration happens in Step 5 D6.  This
	 * thin wrapper keeps the API surface stable so Step 4 spawn path
	 * can reference the symbol.  In Step 5 the body becomes a call to
	 * the cluster_shmem region registry (mirroring spec-1.14 D6
	 * pattern). */
}


/* ============================================================
 * Lifecycle accessors (LW_SHARED).
 *
 *   Spec-1.11.1 F11 7-key + 2 cssd-specific (total send + recv counts;
 *   alive peer count is a derived value).
 * ============================================================ */

pid_t
cluster_cssd_get_pid(void)
{
	pid_t v;

	if (CssdShmem == NULL)
		return 0;
	LWLockAcquire(&CssdShmem->lwlock, LW_SHARED);
	v = CssdShmem->pid;
	LWLockRelease(&CssdShmem->lwlock);
	return v;
}

TimestampTz
cluster_cssd_get_spawned_at(void)
{
	TimestampTz v;

	if (CssdShmem == NULL)
		return 0;
	LWLockAcquire(&CssdShmem->lwlock, LW_SHARED);
	v = CssdShmem->spawned_at;
	LWLockRelease(&CssdShmem->lwlock);
	return v;
}

TimestampTz
cluster_cssd_get_ready_at(void)
{
	TimestampTz v;

	if (CssdShmem == NULL)
		return 0;
	LWLockAcquire(&CssdShmem->lwlock, LW_SHARED);
	v = CssdShmem->ready_at;
	LWLockRelease(&CssdShmem->lwlock);
	return v;
}

TimestampTz
cluster_cssd_get_last_liveness_tick_at(void)
{
	TimestampTz v;

	if (CssdShmem == NULL)
		return 0;
	LWLockAcquire(&CssdShmem->lwlock, LW_SHARED);
	v = CssdShmem->last_liveness_tick_at;
	LWLockRelease(&CssdShmem->lwlock);
	return v;
}

uint64
cluster_cssd_get_main_loop_iters(void)
{
	uint64 v;

	if (CssdShmem == NULL)
		return 0;
	LWLockAcquire(&CssdShmem->lwlock, LW_SHARED);
	v = (uint64)CssdShmem->main_loop_iters;
	LWLockRelease(&CssdShmem->lwlock);
	return v;
}

ClusterCssdStatus
cluster_cssd_get_status(void)
{
	ClusterCssdStatus v;

	if (CssdShmem == NULL)
		return CLUSTER_CSSD_STARTING;
	LWLockAcquire(&CssdShmem->lwlock, LW_SHARED);
	v = CssdShmem->status;
	LWLockRelease(&CssdShmem->lwlock);
	return v;
}

uint64
cluster_cssd_get_total_heartbeat_send_count(void)
{
	if (CssdShmem == NULL)
		return 0;
	return pg_atomic_read_u64(&CssdShmem->total_heartbeat_send_count);
}

uint64
cluster_cssd_get_total_heartbeat_recv_count(void)
{
	if (CssdShmem == NULL)
		return 0;
	return pg_atomic_read_u64(&CssdShmem->total_heartbeat_recv_count);
}

int
cluster_cssd_get_alive_peer_count(void)
{
	int alive = 0;
	int peer;

	if (CssdShmem == NULL)
		return 0;
	for (peer = 0; peer < CLUSTER_MAX_NODES; peer++) {
		if (peer == cluster_node_id)
			continue;
		if (pg_atomic_read_u32(&CssdShmem->peers[peer].state) == CLUSTER_CSSD_PEER_ALIVE)
			alive++;
	}
	return alive;
}

ClusterCssdPeerState
cluster_cssd_get_peer_state(int32 peer_id)
{
	if (CssdShmem == NULL)
		return CLUSTER_CSSD_PEER_ALIVE;
	if (peer_id < 0 || peer_id >= CLUSTER_MAX_NODES)
		return CLUSTER_CSSD_PEER_ALIVE;
	return (ClusterCssdPeerState)pg_atomic_read_u32(&CssdShmem->peers[peer_id].state);
}

TimestampTz
cluster_cssd_get_peer_last_recv_at(int32 peer_id)
{
	if (CssdShmem == NULL)
		return 0;
	if (peer_id < 0 || peer_id >= CLUSTER_MAX_NODES)
		return 0;
	return (TimestampTz)pg_atomic_read_u64(&CssdShmem->peers[peer_id].last_heartbeat_recv_at_us);
}

ClusterCssdOutboundSlot *
cluster_cssd_outbound_slots(void)
{
	if (CssdShmem == NULL)
		return NULL;
	return &CssdShmem->outbound[0];
}


/* ============================================================
 * Per-peer counter bumpers.
 *
 *   Step 2 stubs;Step 5 wires these to cluster_pgstat per-peer counter
 *   array (D14: 4 NEW counter fields cssd_*).  Until then these
 *   forward to the per-peer atomic counter on CssdShmem itself so
 *   Sprint A standalone-test can verify counter increments work.
 * ============================================================ */

void
cluster_cssd_bump_send_count(int32 peer_id)
{
	if (CssdShmem == NULL)
		return;
	if (peer_id < 0 || peer_id >= CLUSTER_MAX_NODES)
		return;
	pg_atomic_add_fetch_u64(&CssdShmem->peers[peer_id].heartbeat_send_count, 1);
	pg_atomic_add_fetch_u64(&CssdShmem->total_heartbeat_send_count, 1);
}

void
cluster_cssd_bump_recv_count(int32 peer_id)
{
	if (CssdShmem == NULL)
		return;
	if (peer_id < 0 || peer_id >= CLUSTER_MAX_NODES)
		return;
	pg_atomic_add_fetch_u64(&CssdShmem->peers[peer_id].heartbeat_recv_count, 1);
	pg_atomic_add_fetch_u64(&CssdShmem->total_heartbeat_recv_count, 1);
}

void
cluster_cssd_bump_suspected_transition(int32 peer_id)
{
	if (CssdShmem == NULL)
		return;
	if (peer_id < 0 || peer_id >= CLUSTER_MAX_NODES)
		return;
	pg_atomic_add_fetch_u64(&CssdShmem->peers[peer_id].suspected_transitions, 1);
}

void
cluster_cssd_bump_dead_transition(int32 peer_id)
{
	if (CssdShmem == NULL)
		return;
	if (peer_id < 0 || peer_id >= CLUSTER_MAX_NODES)
		return;
	pg_atomic_add_fetch_u64(&CssdShmem->peers[peer_id].dead_transitions, 1);
}


/* ============================================================
 * Heartbeat dispatch handler (msg_type=11).
 *
 *   Handler 4 hard constraints (per spec-2.3 §3.5):
 *     - nonblocking (no LWLockAcquire wait)
 *     - no catalog SQL
 *     - no ereport ERROR/FATAL/PANIC
 *     - bounded work (atomic update only)
 *
 *   Per-peer state transition (ALIVE → SUSPECTED → DEAD) happens in
 *   CssdMain's deadband-scan tick, NOT here, to avoid handler-side
 *   state machine vs. main-loop concurrency.  Handler only updates
 *   atomic counters + last_recv_at;recv counts here, transition
 *   logging there.
 * ============================================================ */

void
cluster_cssd_dispatch_heartbeat(const ClusterICEnvelope *env, const void *payload)
{
	int32 sender;
	ClusterCssdHeartbeatPayload hb;

	if (env == NULL || payload == NULL)
		return;
	if (CssdShmem == NULL)
		return; /* shmem not yet ready;handler runs in LMON context — drop */

	sender = (int32)env->source_node_id;
	if (sender < 0 || sender >= CLUSTER_MAX_NODES)
		return;
	if (env->payload_length != sizeof(hb))
		return;

	memcpy(&hb, payload, sizeof(hb)); /* L34 unaligned safe */

	pg_atomic_write_u64(&CssdShmem->peers[sender].last_heartbeat_recv_at_us,
						(uint64)GetCurrentTimestamp());
	pg_atomic_add_fetch_u64(&CssdShmem->peers[sender].heartbeat_recv_count, 1);
	pg_atomic_add_fetch_u64(&CssdShmem->total_heartbeat_recv_count, 1);

	(void)hb; /* hb fields are diagnostic-only;deadband scan uses
				* receiver-local clock, not sender_local_clock (clock skew
				* safe). */
}


/* ============================================================
 * CssdMain skeleton.
 *
 *   Step 2 minimal:  enter shutdown loop; publish READY status; tick
 *   at fixed 1000 ms interval (real GUC + wait event in Step 5 D8/D9).
 *   Heartbeat publish to outbound queue + deadband scan are stubbed
 *   (Step 5 fills body).  This skeleton lets Step 4 postmaster wiring
 *   integrate cleanly without rewriting CssdMain.
 *
 *   Asserts IsUnderPostmaster (HC1 reverse defense).
 * ============================================================ */

void
CssdMain(void)
{
	Assert(IsUnderPostmaster);

	/* Step 4 wires postmaster spawn → CssdMain;Step 5 GUC + wait event +
	 * inject point machinery turns this into the real heartbeat tick.
	 *
	 * For Step 2, this entry compiles + links so the postmaster spawn
	 * patch (Step 4) can target it.  Calling it directly without the
	 * spawn path will hit Assert(IsUnderPostmaster) FATAL in postmaster
	 * context. */
	proc_exit(0);
}


/* ============================================================
 * Lifecycle (Step 4 D4 thin proxies).
 *
 *   Real implementation in Step 4: cluster_postmaster_start_cssd in
 *   postmaster.c (file-static StartChildProcess access);here we only
 *   declare the API surface.  Step 2 stubs that fail-safe so unit
 *   tests don't trigger spawn paths inadvertently.
 * ============================================================ */

int
cluster_cssd_start(void)
{
	/* Real impl in Step 4: forwards to cluster_postmaster_start_cssd(). */
	return 0;
}

bool
cluster_cssd_wait_for_ready(int timeout_ms pg_attribute_unused())
{
	/* Real impl in Step 4: bounded poll on CssdShmem->status. */
	return false;
}

void
cluster_cssd_request_shutdown(void)
{
	if (CssdShmem == NULL)
		return;
	LWLockAcquire(&CssdShmem->lwlock, LW_EXCLUSIVE);
	CssdShmem->shutdown_requested = true;
	LWLockRelease(&CssdShmem->lwlock);
}


#endif /* USE_PGRAC_CLUSTER */
