/*-------------------------------------------------------------------------
 *
 * cluster_undo_alloc.h
 *	  pgrac undo segment allocator (runtime API).
 *
 *	  Stage 1.22 ships:
 *	    - cluster_undo_path_resolve(): build the segment file path
 *	      $PGDATA/pg_undo/instance_<N>/seg_<segment_id>.dat
 *	    - cluster_undo_segment_allocate(): create a segment file +
 *	      initialize block 0 + emit XLOG_UNDO_SEGMENT_INIT
 *
 *	  Stage 1.22 single-node restriction: owner_instance must equal 1
 *	  (cluster_node_id 0 + 1).  Cross-instance allocation rejected
 *	  with ERRCODE_FEATURE_NOT_SUPPORTED until Stage 2+ feature-117.
 *
 *	  Spec: spec-1.22-undo-tablespace-bootstrap.md §2.3 + §D5 + §D6.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/storage/cluster_undo_alloc.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_UNDO_ALLOC_H
#define CLUSTER_UNDO_ALLOC_H

#include "c.h"
#include "storage/block.h"


/*
 * Stage 1.22 single-node owner constant.  cluster_node_id = 0 (default
 * single-node setting) + 1 = 1.  Stage 2+ extends to multi-instance.
 */
#define CLUSTER_UNDO_DEFAULT_OWNER ((uint8)1)


/*
 * cluster_undo_path_resolve
 *	  Build the segment file path:
 *	    $PGDATA/pg_undo/instance_<N>/seg_<segment_id>.dat
 *
 *	  Path components are bounded:
 *	    <N> in [0, 255] (uint8 max)
 *	    <segment_id> in [0, 4294967295] (uint32 max)
 *
 *	  Returns 0 on success, -1 on buffer overflow.  Caller supplies a
 *	  buf with capacity >= MAXPGPATH.
 */
extern int cluster_undo_path_resolve(uint8 instance, uint32 segment_id, char *buf, size_t buf_size);


/*
 * cluster_undo_segment_allocate
 *	  Allocate (create on disk + initialize block 0 + WAL-protect) an
 *	  undo segment file.
 *
 *	  Behavior:
 *	    1. Open or create $PGDATA/pg_undo/instance_<N>/seg_<segment_id>.dat
 *	       with O_CREAT | O_RDWR.  If the file already exists, returns
 *	       silently (idempotent allocation; useful for crash-recovery
 *	       replay paths and Stage 2+ SCN-driven multi-instance).
 *	    2. Use the frontend-safe helper to fill an 8 KB buffer with the
 *	       freshly-allocated UndoSegmentHeaderData layout.
 *	    3. Lock + extend the file to UNDO_SEGMENT_SIZE_BYTES (64 MB).
 *	    4. pwrite the header to block 0 + fsync.
 *	    5. Emit XLOG_UNDO_SEGMENT_INIT (RM_CLUSTER_UNDO) so crash recovery
 *	       reapplies the same byte layout if the page is lost.
 *
 *	  Stage 1.22 limitations:
 *	    - owner_instance MUST equal CLUSTER_UNDO_DEFAULT_OWNER (1);
 *	      cross-instance allocation rejected with FEATURE_NOT_SUPPORTED.
 *	    - segment_size_bytes is hard-coded to UNDO_SEGMENT_SIZE_BYTES;
 *	      retention / sub-segment / extent layout deferred to feature-117.
 *
 *	  Caller MUST NOT be inside a critical section: function emits WAL
 *	  via XLogInsert + may ereport(ERROR) on path / I/O failures.
 */
extern void cluster_undo_segment_allocate(uint32 segment_id, uint8 owner_instance);


#endif /* CLUSTER_UNDO_ALLOC_H */
