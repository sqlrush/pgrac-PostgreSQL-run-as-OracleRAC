/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.c
 *	  Two-phase-commit resource managers tables
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/twophase_rmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/twophase_rmgr.h"
#include "pgstat.h"
#include "storage/lock.h"
#include "storage/predicate.h"

/* PGRAC (spec-3.15 D2): cluster TT 2PC callbacks (enable-cluster only). */
#ifdef USE_PGRAC_CLUSTER
#include "cluster/cluster_tt_2pc.h"
#endif


const TwoPhaseCallback twophase_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_recover,		/* Lock */
	NULL,						/* pgstat */
	multixact_twophase_recover, /* MultiXact */
	predicatelock_twophase_recover	/* PredicateLock */,
#ifdef USE_PGRAC_CLUSTER
	cluster_tt_twophase_recover,	/* ClusterTT */
#else
	NULL,						/* ClusterTT (disable-cluster) */
#endif
};

const TwoPhaseCallback twophase_postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postcommit,	/* Lock */
	pgstat_twophase_postcommit, /* pgstat */
	multixact_twophase_postcommit,	/* MultiXact */
	NULL						/* PredicateLock */,
#ifdef USE_PGRAC_CLUSTER
	cluster_tt_twophase_postcommit,	/* ClusterTT */
#else
	NULL,						/* ClusterTT (disable-cluster) */
#endif
};

const TwoPhaseCallback twophase_postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postabort,	/* Lock */
	pgstat_twophase_postabort,	/* pgstat */
	multixact_twophase_postabort,	/* MultiXact */
	NULL						/* PredicateLock */,
#ifdef USE_PGRAC_CLUSTER
	cluster_tt_twophase_postabort,	/* ClusterTT */
#else
	NULL,						/* ClusterTT (disable-cluster) */
#endif
};

const TwoPhaseCallback twophase_standby_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_standby_recover,	/* Lock */
	NULL,						/* pgstat */
	NULL,						/* MultiXact */
	NULL						/* PredicateLock */,
#ifdef USE_PGRAC_CLUSTER
	NULL, /* ClusterTT: standby rebuild does NOT use this table (spec-3.16 D1) --
		   * the table is dead in PG 16 (no ProcessRecords caller) and
		   * activating it would double-acquire standby AccessExclusiveLocks;
		   * cluster standby overlay rebuild runs as a cluster-only traversal in
		   * twophase.c::ProcessClusterTTStandbyRecover instead. */
#else
	NULL,						/* ClusterTT (standby; disable-cluster) */
#endif
};
