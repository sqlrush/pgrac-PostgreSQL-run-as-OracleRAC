/*-------------------------------------------------------------------------
 *
 * cluster_epoch.c
 *	  pgrac cluster membership epoch shmem state -- single atomic
 *	  uint64 carrying current_epoch, with cache-line padding +
 *	  spec-2.29 reconfig metadata reserved bytes (Q1 修订).
 *
 *	  spec-2.4 期 epoch 永远 = CLUSTER_EPOCH_INITIAL = 0;
 *	  spec-2.29 reconfig 真激活 epoch++ broadcast -- 此 module 当时
 *	  add advance() / set() API + reconfig coordinator hooks.
 *
 *	  Process-local stub variant for --disable-cluster builds:
 *	  cluster_epoch_get_current() returns CLUSTER_EPOCH_INITIAL
 *	  unconditionally, so envelope-build code paths in shared
 *	  module compile portably.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_epoch.c
 *
 * NOTES
 *	  pgrac-original file.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cluster/cluster_epoch.h"

#ifdef USE_PGRAC_CLUSTER

#include "port/atomics.h"
#include "storage/shmem.h"

#include "cluster/cluster_shmem.h" /* cluster_shmem_register_region */

/*
 * spec-2.4 D1 / Q1 修订:
 *
 *   ClusterEpochShmem -- 64-byte cache-line aligned shmem layout.
 *
 *   Field layout:
 *     [0..7]    current_epoch          (8 B atomic;hot read every envelope)
 *     [8..15]   epoch_changed_at_lsn   (8 B atomic;spec-2.29 reconfig writes
 *                                       on epoch++;reserved 0 in spec-2.4)
 *     [16..63]  _reserved              (48 B;spec-2.29 reconfig coordinator
 *                                       metadata land here in place;
 *                                       MUST stay zero in spec-2.4)
 *
 *   Why 64-byte:
 *     1. False sharing防御 -- current_epoch is read on every envelope
 *        build + verify (hot path);if other cluster atomic counters
 *        landed in the same cache line, modifications elsewhere
 *        would invalidate this line on every reader CPU.
 *     2. spec-2.29 reconfig coordinator metadata (pid / quorum_state /
 *        pending_acks_bitmap) gets 48 reserved bytes to extend in
 *        place without cross-file ABI churn.
 *
 *   shmem layout != catalog ABI (postmaster restart rebuilds shmem;
 *   no datadir compatibility implication).
 */
typedef struct ClusterEpochShmem {
	pg_atomic_uint64 current_epoch;
	pg_atomic_uint64 epoch_changed_at_lsn; /* spec-2.29;reserved 0 here */
	uint8 _reserved[48];				   /* spec-2.29 coordinator metadata */
} ClusterEpochShmem;

StaticAssertDecl(sizeof(ClusterEpochShmem) == 64,
				 "ClusterEpochShmem must be exactly 64 bytes (cache-line + reserved)");

static ClusterEpochShmem *cluster_epoch_state = NULL;


Size
cluster_epoch_shmem_size(void)
{
	return sizeof(ClusterEpochShmem);
}

void
cluster_epoch_shmem_init(void)
{
	bool found;

	cluster_epoch_state
		= ShmemInitStruct("pgrac cluster epoch", cluster_epoch_shmem_size(), &found);
	if (!found) {
		pg_atomic_init_u64(&cluster_epoch_state->current_epoch, CLUSTER_EPOCH_INITIAL);
		pg_atomic_init_u64(&cluster_epoch_state->epoch_changed_at_lsn, 0);
		memset(cluster_epoch_state->_reserved, 0, sizeof(cluster_epoch_state->_reserved));
	}
}

uint64
cluster_epoch_get_current(void)
{
	if (cluster_epoch_state == NULL)
		return CLUSTER_EPOCH_INITIAL;
	return pg_atomic_read_u64(&cluster_epoch_state->current_epoch);
}

static const ClusterShmemRegion cluster_epoch_region = {
	.name = "pgrac cluster epoch",
	.size_fn = cluster_epoch_shmem_size,
	.init_fn = cluster_epoch_shmem_init,
	.lwlock_count = 0,
	.owner_subsys = "cluster_epoch",
	.reserved_flags = 0,
};

void
cluster_epoch_shmem_register(void)
{
	cluster_shmem_register_region(&cluster_epoch_region);
}

#else /* !USE_PGRAC_CLUSTER */

/*
 * Disable-cluster stub.  Same symbol surface, returns
 * CLUSTER_EPOCH_INITIAL unconditionally.  Required for envelope
 * code paths that include cluster_epoch.h in disable mode.
 */
uint64
cluster_epoch_get_current(void)
{
	return CLUSTER_EPOCH_INITIAL;
}

#endif /* USE_PGRAC_CLUSTER */
