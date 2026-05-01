/*-------------------------------------------------------------------------
 *
 * cluster_shmem.h
 *	  pgrac cluster shared-memory entry points and control block.
 *
 *	  This header is the single source of truth for cluster shmem C
 *	  declarations.  Stage 0.14 introduces the registration mechanism
 *	  and allocates the first cluster shmem region, ClusterShmemCtl --
 *	  a small immutable control block holding magic, version, and
 *	  startup metadata.  Future cluster shmem regions (GRD, PCM, GES,
 *	  ...) land here at the same time their owning subsystem spec is
 *	  implemented; see docs/cluster-shmem-design.md §3.1 for the
 *	  Single Source of Truth registration roster.
 *
 *	  Responsibilities of this header:
 *
 *	  - Declare the ClusterShmemCtl struct + CLUSTER_SHMEM_MAGIC.
 *	  - Declare the public entry points cluster_request_shmem() and
 *	    cluster_init_shmem(), which PG core (miscinit.c / ipci.c) calls
 *	    under #ifdef USE_PGRAC_CLUSTER during postmaster startup.
 *	  - Declare cluster_shmem_size() for diagnostics.
 *	  - Export the global ClusterShmem pointer so cluster code can read
 *	    metadata fields without going through SQL.
 *
 *	  Stage 0.14 registers ONLY the control block.  ~12 future shmem
 *	  regions (GRD / PCM / GES / Buffer ship / SCN / Sinval / Heartbeat
 *	  / Interconnect / Undo / TT / ADG / Reconfig) are reserved in the
 *	  design doc and registered together with their owning subsystem
 *	  spec (CLAUDE.md rule 8).
 *
 *	  Locking: stage 0.14 control block fields are immutable after
 *	  postmaster init (no concurrent writes), so no LWLock is required.
 *	  The first subsystem that needs concurrent shmem writes registers
 *	  its own LWLock tranche at that time.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_shmem.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Includes datatype/timestamp.h for TimestampTz; otherwise PG-free
 *	  at the declaration level.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_SHMEM_H
#define CLUSTER_SHMEM_H

#include "c.h"					/* Size */
#include "datatype/timestamp.h" /* TimestampTz */


/*
 * CLUSTER_SHMEM_MAGIC -- "PGRC" little-endian.
 *
 *	First 4 bytes of every cluster shmem region.  Readers verify the
 *	magic before trusting the rest of the structure; mismatch usually
 *	means stale shmem segment from a previous binary.
 */
#define CLUSTER_SHMEM_MAGIC 0x50475243UL


/*
 * ClusterShmemCtl -- top-level cluster control block.
 *
 *	One instance per postmaster, allocated by cluster_init_shmem() and
 *	never written again.  Holds version + identity metadata visible to
 *	any cluster code (e.g. for sanity checks during cross-process
 *	communication).
 *
 *	When future subsystems (GRD, PCM, ...) need their own shmem regions,
 *	they allocate independent ShmemInitStruct entries -- they do NOT
 *	embed mutable fields here, because mutable state would require
 *	locking that the control block intentionally avoids.
 */
typedef struct ClusterShmemCtl {
	uint32 magic;			/* CLUSTER_SHMEM_MAGIC */
	uint32 version_packed;	/* (major<<24)|(minor<<16)|(patch<<8)|stage_step */
	int32 node_id_at_init;	/* snapshot of cluster_node_id GUC at init */
	int32 _padding;			/* keep created_at 8-byte aligned */
	TimestampTz created_at; /* GetCurrentTimestamp() at init */
} ClusterShmemCtl;


/*
 * Public entry points called by PG core under #ifdef USE_PGRAC_CLUSTER.
 *
 *	cluster_request_shmem() must run inside process_shmem_requests()
 *	(miscinit.c) so that PG accepts the RequestAddinShmemSpace() call.
 *	cluster_init_shmem() must run inside CreateSharedMemoryAndSemaphores()
 *	(ipci.c) after PG's built-in ShmemInit calls and before the user
 *	shmem_startup_hook fires.
 *
 *	cluster_shmem_size() is a helper that returns the total byte count
 *	the cluster subsystem will request -- useful for diagnostics and
 *	future capacity planning.
 */
extern void cluster_request_shmem(void);
extern void cluster_init_shmem(void);
extern Size cluster_shmem_size(void);


/*
 * Process-local pointer to the cluster control block.
 *
 *	NULL until cluster_init_shmem() runs in this process.  After that,
 *	it points into the shared memory segment and stays valid for the
 *	process lifetime.  Never written after initialisation.
 */
extern ClusterShmemCtl *ClusterShmem;


/*
 * ClusterShmemRegion -- registration entry for a cluster shmem region.
 *
 *	Each cluster subsystem (cluster_ctl + cluster_conf in stage 1.3;
 *	grd / pcm / ges / ... in stage 2+) declares a static const
 *	ClusterShmemRegion struct and registers it via
 *	cluster_shmem_register_region during postmaster init (after
 *	cluster_init_guc, before cluster_request_shmem).
 *
 *	Lifetime:
 *	  register-time: cluster_init_guc() done; registry not yet frozen
 *	  freeze-time:   cluster_request_shmem() entry sets registry frozen
 *	  request-time:  cluster_request_shmem() iterates and calls size_fn
 *	                 per row, passing the result to RequestAddinShmemSpace
 *	  init-time:     cluster_init_shmem() iterates and calls init_fn
 *	                 per row (init_fn must use ShmemInitStruct so that
 *	                 EXEC_BACKEND children rebind the existing region)
 *
 *	See docs/cluster-shmem-design.md §9 for the full registry design and
 *	specs/spec-1.3-shmem-region-registry.md §2.1 for field semantics.
 */
typedef struct ClusterShmemRegion {
	const char *name;		  /* unique within registry */
	Size (*size_fn)(void);	  /* returns bytes; called in Phase 1 */
	void (*init_fn)(void);	  /* called in Phase 2; idempotent (EXEC_BACKEND found=true) */
	int lwlock_count;		  /* informational; subsystem registers LWLocks itself */
	const char *owner_subsys; /* "cluster_ctl" / "cluster_conf" / "grd" / ... */
	int reserved_flags;		  /* future extension; must be 0 */
} ClusterShmemRegion;


/*
 * Region registry API (since stage 1.3).
 *
 *	cluster_shmem_register_region:
 *	  Register one region.  Must be called after cluster_init_guc and
 *	  before cluster_request_shmem.  Errors:
 *	    - registry frozen          -> ERROR (FEATURE_NOT_SUPPORTED 0A000)
 *	    - duplicate name           -> ERROR (DUPLICATE_OBJECT 42710)
 *	    - capacity exceeded        -> ERROR (CONFIG_LIMIT_EXCEEDED 53400)
 *
 *	cluster_shmem_lookup_region:
 *	  Lookup a region by name.  Returns NULL if not found.
 *
 *	cluster_shmem_iter_regions:
 *	  Iterator-style traversal.  Caller initializes *idx = 0 and calls
 *	  repeatedly; returns true and fills *out per call; returns false
 *	  when exhausted.  Used by SRF and pg_cluster_state dump.
 *
 *	cluster_shmem_get_region_count:
 *	  Returns the number of registered regions (for diagnostics).
 *
 *	cluster_shmem_get_total_bytes:
 *	  Returns the sum of size_fn() over all registered regions.
 */
extern void cluster_shmem_register_region(const ClusterShmemRegion *region);
extern const ClusterShmemRegion *cluster_shmem_lookup_region(const char *name);
extern bool cluster_shmem_iter_regions(int *idx, ClusterShmemRegion *out);
extern int cluster_shmem_get_region_count(void);
extern Size cluster_shmem_get_total_bytes(void);


/*
 * cluster_init_shmem_module -- register foundational regions.
 *
 *	Called once from cluster_init() during postmaster startup, after
 *	cluster_init_guc has completed.  Allocates the registry array
 *	(cluster_shmem_max_regions slots) and registers the cluster_ctl
 *	and cluster_conf regions.  Other subsystems register their own
 *	regions from their own init functions (cluster_ic_init, etc.) --
 *	this helper only owns the foundational two.
 */
extern void cluster_init_shmem_module(void);


#endif /* CLUSTER_SHMEM_H */
