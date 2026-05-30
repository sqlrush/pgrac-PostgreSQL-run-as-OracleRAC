/*-------------------------------------------------------------------------
 *
 * cluster_cr.c
 *	  pgrac own-instance Consistent Read (CR) block construction.
 *
 *	  Stage 3 第 13 sub-spec (spec-3.9).  Top-level CR machinery:
 *	    - backend-local 8 KB scratch slot + non-reentrant guard
 *	    - ClusterCRShared shmem region (9 atomic counters)
 *	    - 2-layer API: cluster_cr_lookup_or_construct (top, spec-3.10 cache
 *	      hook) / cluster_cr_construct_block (bottom, always constructs)
 *	    - chain walker driver (Step 3) + tuple remap + CR-image visibility
 *	      helper (Step 4.5)
 *
 *	  Inverse-apply helpers live in cluster_cr_apply.c (Step 4).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * Spec: spec-3.9-own-instance-cr-block-construction.md (FROZEN v0.4)
 *
 * IDENTIFICATION
 *	  src/backend/cluster/cluster_cr.c
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/shmem.h"
#include "utils/elog.h"
#include "utils/memutils.h"

#include "cluster/cluster_cr.h"
#include "cluster/cluster_shmem.h"


/*
 * ClusterCRShared -- per-instance shmem counters (spec-3.9 §2.5).
 *
 *	9 atomic counters, no LWLock (region lwlock_count = 0): counters are
 *	bumped with pg_atomic_fetch_add_u64 from the constructing backend,
 *	read lock-free by dump_state / pg_cluster_state.
 */
typedef struct ClusterCRShared {
	pg_atomic_uint64 cr_construct_count;
	pg_atomic_uint64 cr_snapshot_too_old_count;
	pg_atomic_uint64 cr_cross_instance_unsupported_count;
	pg_atomic_uint64 cr_corruption_count;
	pg_atomic_uint64 cr_chain_walk_steps_sum;
	pg_atomic_uint64 cr_inverse_insert_count;
	pg_atomic_uint64 cr_inverse_update_count;
	pg_atomic_uint64 cr_inverse_delete_count;
	pg_atomic_uint64 cr_inverse_itl_count;
} ClusterCRShared;

static ClusterCRShared *CRShared = NULL;

/*
 * Backend-local CR scratch slot (spec-3.9 Q3 / I-cr-1).
 *
 *	Single 8 KB reusable page allocated once in TopMemoryContext on first
 *	construction.  cluster_cr_construct_block returns a pointer into this
 *	slot; the pointer is valid only until the next construction call.
 *
 *	cr_in_progress is the non-reentrant guard (I-lock-3): nested CR
 *	construction in the same backend would clobber the shared scratch, so
 *	we Assert against it and keep the flag balanced across ereport via
 *	PG_TRY/PG_CATCH.
 */
static char *cr_scratch = NULL;
static bool cr_in_progress = false;


/* ============================================================
 * Shmem region (L206 5-step)
 * ============================================================ */

Size
cluster_cr_shmem_size(void)
{
	return MAXALIGN(sizeof(ClusterCRShared));
}

void
cluster_cr_shmem_init(void)
{
	bool found;

	CRShared = ShmemInitStruct("ClusterCRShared", cluster_cr_shmem_size(), &found);

	if (!found) {
		pg_atomic_init_u64(&CRShared->cr_construct_count, 0);
		pg_atomic_init_u64(&CRShared->cr_snapshot_too_old_count, 0);
		pg_atomic_init_u64(&CRShared->cr_cross_instance_unsupported_count, 0);
		pg_atomic_init_u64(&CRShared->cr_corruption_count, 0);
		pg_atomic_init_u64(&CRShared->cr_chain_walk_steps_sum, 0);
		pg_atomic_init_u64(&CRShared->cr_inverse_insert_count, 0);
		pg_atomic_init_u64(&CRShared->cr_inverse_update_count, 0);
		pg_atomic_init_u64(&CRShared->cr_inverse_delete_count, 0);
		pg_atomic_init_u64(&CRShared->cr_inverse_itl_count, 0);
	}
}

static const ClusterShmemRegion cluster_cr_region = {
	.name = "pgrac cluster cr counters",
	.size_fn = cluster_cr_shmem_size,
	.init_fn = cluster_cr_shmem_init,
	.lwlock_count = 0, /* atomic counters only; no LWLock */
	.owner_subsys = "cluster_cr",
	.reserved_flags = 0,
};

void
cluster_cr_shmem_register(void)
{
	cluster_shmem_register_region(&cluster_cr_region);
}


/* ============================================================
 * Counter accessors (spec-3.9 §2.1)
 * ============================================================ */

#define CR_COUNTER_ACCESSOR(fn, field)                                                             \
	uint64 fn(void)                                                                                \
	{                                                                                              \
		if (CRShared == NULL)                                                                      \
			return 0;                                                                              \
		return pg_atomic_read_u64(&CRShared->field);                                               \
	}

CR_COUNTER_ACCESSOR(cluster_cr_construct_count, cr_construct_count)
CR_COUNTER_ACCESSOR(cluster_cr_snapshot_too_old_count, cr_snapshot_too_old_count)
CR_COUNTER_ACCESSOR(cluster_cr_cross_instance_unsupported_count,
					cr_cross_instance_unsupported_count)
CR_COUNTER_ACCESSOR(cluster_cr_corruption_count, cr_corruption_count)
CR_COUNTER_ACCESSOR(cluster_cr_chain_walk_steps_sum, cr_chain_walk_steps_sum)
CR_COUNTER_ACCESSOR(cluster_cr_inverse_insert_count, cr_inverse_insert_count)
CR_COUNTER_ACCESSOR(cluster_cr_inverse_update_count, cr_inverse_update_count)
CR_COUNTER_ACCESSOR(cluster_cr_inverse_delete_count, cr_inverse_delete_count)
CR_COUNTER_ACCESSOR(cluster_cr_inverse_itl_count, cr_inverse_itl_count)


/* ============================================================
 * Scratch slot helpers
 * ============================================================ */

/*
 * Ensure the backend-local scratch page is allocated (once, in
 * TopMemoryContext so it survives the lifetime of the backend).
 */
static void
cr_scratch_ensure(void)
{
	if (cr_scratch == NULL)
		cr_scratch = MemoryContextAllocZero(TopMemoryContext, BLCKSZ);
}


/* ============================================================
 * Chain walker driver (body lands in Step 3)
 * ============================================================ */

/*
 * cr_walk_and_apply -- walk ITL[itl_idx].uba_head backward and inverse-apply
 *	every undo record newer than read_scn onto scratch_page.
 *
 *	Step 2 ships the signature + non-reentrant integration only; the real
 *	chain walk + SCN stop condition + chain terminal taxonomy + step cap
 *	land in Step 3 (spec-3.9 D3).  Explicit FEATURE_NOT_SUPPORTED keeps the
 *	intermediate commit honest (CLAUDE.md 规则 8) — nothing calls
 *	cluster_cr_construct_block until Step 5 (heapam_visibility integration).
 */
static void
cr_walk_and_apply(char *scratch_page, Buffer buf, SCN read_scn, int itl_idx)
{
	(void)scratch_page;
	(void)buf;
	(void)read_scn;
	(void)itl_idx;

	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cluster CR chain walker not yet implemented"),
					errhint("Lands in spec-3.9 Step 3 (D3); this path is not reachable "
							"until Step 5 visibility integration.")));
}


/* ============================================================
 * 2-layer public API
 * ============================================================ */

const char *
cluster_cr_construct_block(Buffer buf, SCN read_scn, int itl_idx)
{
	const char *result;

	Assert(BufferIsValid(buf));
	Assert(!cr_in_progress); /* I-lock-3 non-reentrant */

	cr_scratch_ensure();

	/*
	 * I-lock-3 + I-fail-1: the guard MUST be balanced even if the chain
	 * walker ereport(ERROR)s, otherwise the next construction Asserts /
	 * the guard wedges this backend.  PG_TRY/PG_CATCH resets the flag and
	 * re-throws so the caller still sees the precise SQLSTATE.
	 */
	cr_in_progress = true;

	PG_TRY();
	{
		/* I-lock-1/2/4: caller holds the content lock; we only read the
		 * page bytes into backend-local scratch — no buffer lock, no WAL,
		 * no dirty. */
		memcpy(cr_scratch, BufferGetPage(buf), BLCKSZ);

		cr_walk_and_apply(cr_scratch, buf, read_scn, itl_idx);

		if (CRShared != NULL)
			pg_atomic_fetch_add_u64(&CRShared->cr_construct_count, 1);

		result = cr_scratch;
	}
	PG_CATCH();
	{
		cr_in_progress = false;
		PG_RE_THROW();
	}
	PG_END_TRY();

	cr_in_progress = false;
	return result;
}

const char *
cluster_cr_lookup_or_construct(Buffer buf, SCN read_scn, int itl_idx)
{
	/*
	 * spec-3.9: fall-through to construction.  spec-3.10 inserts the CR
	 * cache lookup here (miss → construct + cache, hit → cached image),
	 * without touching the visibility caller.
	 */
	return cluster_cr_construct_block(buf, read_scn, itl_idx);
}


/* ============================================================
 * Tuple remap (CR scratch page → HeapTupleData wrapper)
 * ============================================================ */

bool
cluster_cr_remap_tuple(const char *cr_page, OffsetNumber off, HeapTupleData *out_htup)
{
	Page page = (Page)cr_page;
	ItemId itemid;

	if (cr_page == NULL || out_htup == NULL)
		return false;
	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return false;

	itemid = PageGetItemId(page, off);
	if (!ItemIdIsNormal(itemid))
		return false; /* LP_UNUSED / LP_DEAD / LP_REDIRECT: CR-removed or not a tuple */

	out_htup->t_len = ItemIdGetLength(itemid);
	out_htup->t_data = (HeapTupleHeader)PageGetItem(page, itemid);
	out_htup->t_tableOid = InvalidOid;
	ItemPointerSetOffsetNumber(&out_htup->t_self, off);
	/* Block number of t_self is filled by the caller (it knows the buffer's
	 * block); CR scratch alone does not carry it. */
	return true;
}
