/*-------------------------------------------------------------------------
 *
 * cluster_remote_xact.c
 *	  Per-origin materialized transaction outcomes (spec-4.5a G5, D10).
 *
 *	  See cluster_remote_xact.h for the design contract.  Mechanically
 *	  this is a small SLRU in the commit_ts mould: fixed-width entries,
 *	  xid-derived page numbers, one control LWLock from a named tranche,
 *	  buffer locks from a dynamically allocated tranche whose id lives in
 *	  shmem so EXEC_BACKEND children register the same id.
 *
 *	  Writers: ONLY the startup process, inside merged replay (single
 *	  threaded).  Readers: any backend after recovery, through
 *	  cluster_remote_commit_outcome.  A missing segment/page reads as
 *	  INDOUBT -- the fail-closed default for "this node never
 *	  materialized that origin's outcome".
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_remote_xact.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-4.5a-shared-storage-data-backend.md (FROZEN v1.0, D10)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/sync.h"
#include "utils/elog.h"

#include "cluster/cluster_elog.h"
#include "cluster/cluster_remote_xact.h"

#ifdef USE_PGRAC_CLUSTER

/*
 * Entry layout: 16 bytes (8B SCN + 1B status + 7B reserved/zero), 512
 * entries per BLCKSZ page.  Page space: origin (7 bits, CLUSTER_MAX_NODES
 * = 128) << 23 | xid-page (2^32 xids / 512 = 2^23 pages) -- 30 bits, a
 * positive int pageno.
 */
typedef struct ClusterRemoteXactEntry {
	SCN commit_scn; /* valid iff status == COMMITTED */
	uint8 status;	/* ClusterRemoteXactOutcome value */
	uint8 _zero[7]; /* reserved; zero (entry version/epoch bits live here
					   * if a future store must span xid epochs) */
} ClusterRemoteXactEntry;

StaticAssertDecl(sizeof(ClusterRemoteXactEntry) == CLUSTER_REMOTE_XACT_ENTRY_BYTES,
				 "remote-xact entry must be 16 bytes");

static SlruCtlData ClusterRemoteXactCtlData;
#define ClusterRemoteXactCtl (&ClusterRemoteXactCtlData)

#define CLUSTER_REMOTE_XACT_BUFFERS 8

typedef struct ClusterRemoteXactShared {
	int buffer_tranche_id; /* allocated once by the postmaster */
	pg_atomic_uint64 diverted_commit_count;
	pg_atomic_uint64 diverted_abort_count;
	pg_atomic_uint64 outcome_indoubt_count;
} ClusterRemoteXactShared;

static ClusterRemoteXactShared *RemoteXactShared = NULL;

static bool
remote_xact_page_precedes(int page1, int page2)
{
	/* No wraparound truncation in this store; plain ordering suffices. */
	return page1 < page2;
}

Size
cluster_remote_xact_shmem_size(void)
{
	return add_size(SimpleLruShmemSize(CLUSTER_REMOTE_XACT_BUFFERS, 0),
					MAXALIGN(sizeof(ClusterRemoteXactShared)));
}

void
cluster_remote_xact_shmem_request(void)
{
	RequestNamedLWLockTranche("ClusterRemoteXactSLRU", 1);
}

void
cluster_remote_xact_shmem_init(void)
{
	bool found;
	LWLock *ctl_lock = &(GetNamedLWLockTranche("ClusterRemoteXactSLRU"))[0].lock;

	RemoteXactShared = (ClusterRemoteXactShared *)ShmemInitStruct(
		"ClusterRemoteXact shared", sizeof(ClusterRemoteXactShared), &found);
	if (!IsUnderPostmaster) {
		Assert(!found);
		RemoteXactShared->buffer_tranche_id = LWLockNewTrancheId();
		pg_atomic_init_u64(&RemoteXactShared->diverted_commit_count, 0);
		pg_atomic_init_u64(&RemoteXactShared->diverted_abort_count, 0);
		pg_atomic_init_u64(&RemoteXactShared->outcome_indoubt_count, 0);
	} else {
		Assert(found);
	}
	LWLockRegisterTranche(RemoteXactShared->buffer_tranche_id, "ClusterRemoteXactBuffer");

	ClusterRemoteXactCtl->PagePrecedes = remote_xact_page_precedes;
	SimpleLruInit(ClusterRemoteXactCtl, "ClusterRemoteXact", CLUSTER_REMOTE_XACT_BUFFERS, 0,
				  ctl_lock, "pg_xact_remote", RemoteXactShared->buffer_tranche_id,
				  SYNC_HANDLER_NONE);
}

/*
 * Fetch (read-locking) the page slot for (origin, xid), materializing a
 * zero page when absent and `create` is true.  Returns slotno with the
 * control lock HELD (exclusive); caller releases.
 */
static int
remote_xact_open_page(int origin_node, TransactionId xid, bool create)
{
	int pageno = cluster_remote_xact_pageno(origin_node, xid);

	LWLockAcquire(ClusterRemoteXactCtl->shared->ControlLock, LW_EXCLUSIVE);
	if (!create && !SimpleLruDoesPhysicalPageExist(ClusterRemoteXactCtl, pageno)) {
		/* Probe-only miss: keep the lock-held contract, caller maps to
		 * INDOUBT after releasing. */
		return -1;
	}
	if (create && !SimpleLruDoesPhysicalPageExist(ClusterRemoteXactCtl, pageno))
		return SimpleLruZeroPage(ClusterRemoteXactCtl, pageno);
	return SimpleLruReadPage(ClusterRemoteXactCtl, pageno, true, xid);
}

/*
 * cluster_remote_xact_set -- record one outcome (startup process only).
 */
static void
cluster_remote_xact_set(int origin_node, TransactionId xid, ClusterRemoteXactOutcome outcome,
						SCN commit_scn)
{
	int slotno;
	ClusterRemoteXactEntry *entry;

	Assert(!IsUnderPostmaster || AmStartupProcess());
	Assert(origin_node >= 0 && origin_node < (1 << 7));

	slotno = remote_xact_open_page(origin_node, xid, true);
	entry = (ClusterRemoteXactEntry *)(ClusterRemoteXactCtl->shared->page_buffer[slotno]
									   + (Size)cluster_remote_xact_entryno(xid)
											 * sizeof(ClusterRemoteXactEntry));
	entry->commit_scn = (outcome == CLUSTER_REMOTE_XACT_COMMITTED) ? commit_scn : InvalidScn;
	entry->status = (uint8)outcome;
	ClusterRemoteXactCtl->shared->page_dirty[slotno] = true;
	LWLockRelease(ClusterRemoteXactCtl->shared->ControlLock);
}

ClusterRemoteXactOutcome
cluster_remote_commit_outcome(int origin_node, TransactionId xid, SCN *commit_scn)
{
	int slotno;
	const ClusterRemoteXactEntry *entry;
	ClusterRemoteXactOutcome outcome;
	SCN scn;

	if (commit_scn != NULL)
		*commit_scn = InvalidScn;
	if (RemoteXactShared == NULL || origin_node < 0 || origin_node >= (1 << 7)
		|| !TransactionIdIsNormal(xid)) {
		if (RemoteXactShared != NULL)
			pg_atomic_fetch_add_u64(&RemoteXactShared->outcome_indoubt_count, 1);
		return CLUSTER_REMOTE_XACT_INDOUBT;
	}

	slotno = remote_xact_open_page(origin_node, xid, false);
	if (slotno < 0) {
		LWLockRelease(ClusterRemoteXactCtl->shared->ControlLock);
		pg_atomic_fetch_add_u64(&RemoteXactShared->outcome_indoubt_count, 1);
		return CLUSTER_REMOTE_XACT_INDOUBT;
	}
	entry = (const ClusterRemoteXactEntry *)(ClusterRemoteXactCtl->shared->page_buffer[slotno]
											 + (Size)cluster_remote_xact_entryno(xid)
												   * sizeof(ClusterRemoteXactEntry));
	outcome = (ClusterRemoteXactOutcome)entry->status;
	scn = entry->commit_scn;
	LWLockRelease(ClusterRemoteXactCtl->shared->ControlLock);

	if (outcome == CLUSTER_REMOTE_XACT_COMMITTED) {
		if (!SCN_VALID(scn)) {
			/* A committed entry without an SCN is corrupt -- fail closed. */
			pg_atomic_fetch_add_u64(&RemoteXactShared->outcome_indoubt_count, 1);
			return CLUSTER_REMOTE_XACT_INDOUBT;
		}
		if (commit_scn != NULL)
			*commit_scn = scn;
		return CLUSTER_REMOTE_XACT_COMMITTED;
	}
	if (outcome == CLUSTER_REMOTE_XACT_ABORTED)
		return CLUSTER_REMOTE_XACT_ABORTED;

	pg_atomic_fetch_add_u64(&RemoteXactShared->outcome_indoubt_count, 1);
	return CLUSTER_REMOTE_XACT_INDOUBT;
}

void
cluster_remote_xact_flush(void)
{
	if (RemoteXactShared == NULL)
		return;
	SimpleLruWriteAll(ClusterRemoteXactCtl, true);
}

/*
 * cluster_remote_xact_apply -- D10b divert (P1-1 fail-closed parse).
 */
void
cluster_remote_xact_apply(int origin_node, XLogReaderState *record)
{
	RmgrId rmid = XLogRecGetRmid(record);
	uint8 info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;
	TransactionId xid = XLogRecGetXid(record);

	if (rmid == RM_CLOG_ID) {
		/*
		 * CLOG records are page-extend/truncate housekeeping for B's OWN
		 * pg_xact.  This node's remote store derives outcomes from the
		 * XACT records themselves, so B's clog page maintenance carries
		 * no information here -- consume it without touching the LOCAL
		 * pg_xact (the whole point of the divert).
		 */
		return;
	}

	Assert(rmid == RM_XACT_ID);

	switch (info) {
	case XLOG_XACT_COMMIT: {
		xl_xact_commit *xlrec = (xl_xact_commit *)XLogRecGetData(record);
		xl_xact_parsed_commit parsed;

		ParseCommitRecord(XLogRecGetInfo(record), xlrec, &parsed);

		/*
		 * P1-1 hard rule: only a pure outcome may materialize.  Every
		 * cross-instance side effect xact_redo_commit would have driven
		 * (relfile drops, invalidations, stats drops, subxacts, 2PC,
		 * AE locks) is unsupported on a foreign stream -- fail closed,
		 * never silently dropped.
		 */
		if (cluster_remote_xact_commit_blocked(parsed.nrels, parsed.nmsgs, parsed.nstats,
											   parsed.nsubxacts, parsed.xinfo,
											   XACT_XINFO_HAS_TWOPHASE, XACT_XINFO_HAS_AE_LOCKS))
			ereport(
				FATAL,
				(errcode(ERRCODE_CLUSTER_MERGED_RECOVERY_BLOCKED),
				 errmsg("merged recovery: foreign commit record carries an unsupported "
						"side effect (origin node %d, xid %u)",
						origin_node, xid),
				 errdetail("nrels=%d nmsgs=%d nstats=%d nsubxacts=%d xinfo=0x%x.", parsed.nrels,
						   parsed.nmsgs, parsed.nstats, parsed.nsubxacts, (unsigned)parsed.xinfo),
				 errhint("Cross-instance relfile drop / invalidation / stats / subxacts / "
						 "2PC are not yet mergeable (roadmap 4.6/4.7 + feature #11); "
						 "recover with cluster.merged_recovery=off.")));

		if (!SCN_VALID(parsed.scn))
			ereport(FATAL, (errcode(ERRCODE_CLUSTER_MERGED_RECOVERY_BLOCKED),
							errmsg("merged recovery: foreign commit record carries no SCN "
								   "(origin node %d, xid %u)",
								   origin_node, xid),
							errhint("spec-1.18 stamps every commit record; a missing SCN means "
									"a pre-cluster WAL stream, which cannot be merged.")));

		cluster_remote_xact_set(origin_node, xid, CLUSTER_REMOTE_XACT_COMMITTED, parsed.scn);
		pg_atomic_fetch_add_u64(&RemoteXactShared->diverted_commit_count, 1);
		break;
	}
	case XLOG_XACT_ABORT: {
		xl_xact_abort *xlrec = (xl_xact_abort *)XLogRecGetData(record);
		xl_xact_parsed_abort parsed;

		ParseAbortRecord(XLogRecGetInfo(record), xlrec, &parsed);
		if (parsed.nrels > 0 || parsed.nstats > 0 || parsed.nsubxacts > 0
			|| (parsed.xinfo & XACT_XINFO_HAS_TWOPHASE) != 0)
			ereport(FATAL, (errcode(ERRCODE_CLUSTER_MERGED_RECOVERY_BLOCKED),
							errmsg("merged recovery: foreign abort record carries an unsupported "
								   "side effect (origin node %d, xid %u)",
								   origin_node, xid),
							errhint("Recover with cluster.merged_recovery=off.")));

		cluster_remote_xact_set(origin_node, xid, CLUSTER_REMOTE_XACT_ABORTED, InvalidScn);
		pg_atomic_fetch_add_u64(&RemoteXactShared->diverted_abort_count, 1);
		break;
	}
	default:
		/*
		 * COMMIT_PREPARED / ABORT_PREPARED / PREPARE / ASSIGNMENT /
		 * INVALIDATIONS: all carry cross-instance machinery this store
		 * cannot honestly absorb yet.
		 */
		ereport(FATAL, (errcode(ERRCODE_CLUSTER_MERGED_RECOVERY_BLOCKED),
						errmsg("merged recovery: unsupported foreign xact record (info 0x%02x, "
							   "origin node %d, xid %u)",
							   (unsigned)info, origin_node, xid),
						errhint("2PC / assignment / standalone-invalidation records are not yet "
								"mergeable; recover with cluster.merged_recovery=off.")));
	}
}

uint64
cluster_remote_xact_diverted_commit_count(void)
{
	return RemoteXactShared ? pg_atomic_read_u64(&RemoteXactShared->diverted_commit_count) : 0;
}
uint64
cluster_remote_xact_diverted_abort_count(void)
{
	return RemoteXactShared ? pg_atomic_read_u64(&RemoteXactShared->diverted_abort_count) : 0;
}
uint64
cluster_remote_xact_outcome_indoubt_count(void)
{
	return RemoteXactShared ? pg_atomic_read_u64(&RemoteXactShared->outcome_indoubt_count) : 0;
}

#endif /* USE_PGRAC_CLUSTER */
