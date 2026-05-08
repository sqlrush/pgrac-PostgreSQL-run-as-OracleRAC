/*-------------------------------------------------------------------------
 *
 * cluster_epoch.h
 *	  pgrac cluster membership epoch -- shmem-backed monotonic counter
 *	  carried by every cross-node IC envelope (Invariant 2 per
 *	  spec-2.0 §3.2).
 *
 *	  spec-2.4 期 epoch 永远 = CLUSTER_EPOCH_INITIAL = 0;
 *	  spec-2.29 reconfig 真激活 epoch++ broadcast.
 *
 *	  Shmem layout (per Q1 修订):
 *	    - 64-byte cache-line padding to prevent false sharing on the
 *	      hot atomic-read path (every envelope build + verify reads
 *	      current_epoch);
 *	    - 48 bytes reserved for spec-2.29 reconfig coordinator
 *	      metadata (pid / quorum_state / pending_acks_bitmap) so
 *	      reconfig can extend in place without cross-file ABI churn.
 *
 *	  shmem layout != catalog ABI:postmaster restart rebuilds shmem,
 *	  so shmem size / field changes do NOT require catversion bump.
 *	  Catversion is reserved for pg_proc / pg_class / system view
 *	  surface.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_epoch.h
 *
 * NOTES
 *	  pgrac-original file.  Built only in --enable-cluster mode
 *	  (USE_PGRAC_CLUSTER);disable-cluster builds get a stub
 *	  cluster_epoch_get_current that returns CLUSTER_EPOCH_INITIAL
 *	  unconditionally so envelope-build code paths stay portable.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_EPOCH_H
#define CLUSTER_EPOCH_H

#include "c.h"

/*
 * Initial epoch value used at boot.  spec-2.4 期所有 envelope 写
 * CLUSTER_EPOCH_INITIAL;spec-2.29 reconfig 是首个真 ++ 该值的 spec.
 */
#define CLUSTER_EPOCH_INITIAL ((uint64)0)

/*
 * Snapshot read of the current cluster membership epoch.  Atomic
 * pg_atomic_read_u64 -- never blocks, safe in critical sections,
 * safe in handler / dispatch / send paths.
 */
extern uint64 cluster_epoch_get_current(void);

/*
 * Shmem registration.  Called from cluster_init_shmem() in
 * postmaster phase 1.  Size returned by cluster_epoch_shmem_size
 * is added to the cluster-subsystem shmem RequestAddinShmemSpace
 * accumulation.
 */
extern Size cluster_epoch_shmem_size(void);
extern void cluster_epoch_shmem_init(void);
extern void cluster_epoch_shmem_register(void);

#endif /* CLUSTER_EPOCH_H */
