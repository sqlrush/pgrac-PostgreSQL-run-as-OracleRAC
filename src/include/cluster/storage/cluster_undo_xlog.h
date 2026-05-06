/*-------------------------------------------------------------------------
 *
 * cluster_undo_xlog.h
 *	  pgrac undo segment WAL resource manager (RM_CLUSTER_UNDO).
 *
 *	  Stage 1.22 ships ONE subtype: XLOG_UNDO_SEGMENT_INIT (info=0x10),
 *	  emitted when cluster_undo_segment_allocate writes block 0 of a
 *	  fresh seg_<id>.dat file.  redo handler reads the payload and
 *	  pwrites the page image directly to the segment file (no PG buffer
 *	  manager / smgr involvement -- pg_undo/instance_<N>/seg_<id>.dat
 *	  is not in PG's RelFileLocator namespace).
 *
 *	  feature-117 / feature-120 may add additional subtypes (retention
 *	  / recycling / TT slot bind / commit_scn write) to the same RM
 *	  without breaking the spec-1.22 ABI.  Reserve high info bits
 *	  (0x20-0xF0) for those.
 *
 *	  Spec: spec-1.22-undo-tablespace-bootstrap.md §2 + §3.6 + §D14a
 *	  (v0.2 P1-A B-lite修订).
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/storage/cluster_undo_xlog.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_UNDO_XLOG_H
#define CLUSTER_UNDO_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"


/*
 * RM info bit values for RM_CLUSTER_UNDO records.
 *
 *   Stage 1.22: XLOG_UNDO_SEGMENT_INIT only.  feature-117/120 may add:
 *     0x20 XLOG_UNDO_TT_SLOT_BIND
 *     0x30 XLOG_UNDO_TT_SLOT_COMMIT
 *     0x40 XLOG_UNDO_SEGMENT_RECYCLE
 *     ... up to 0xF0 (low nibble used for XLR_INFO_MASK by xlog framework).
 */
#define XLOG_UNDO_SEGMENT_INIT 0x10


/*
 * On-disk WAL payload layout for XLOG_UNDO_SEGMENT_INIT.
 *
 *   payload = 6-byte fixed header + BLCKSZ-byte page image.
 *   Stored sequentially in a single XLogRegisterData block.
 *
 *   v0.2 P1-A: payload uses a custom path-based identification
 *   (instance + segment_id) rather than RelFileLocator/BlockNumber
 *   because pg_undo/ files live outside PG's relfilenode namespace.
 *   redo handler resolves the path via cluster_undo_path_resolve()
 *   and pwrites directly.
 */
typedef struct xl_cluster_undo_segment_init {
	uint8 instance;	   /* owner instance (1..128); spec-1.21 Q5 */
	uint8 _pad[1];	   /* alignment pad */
	uint16 _pad2[2];   /* alignment pad to 8 bytes */
	uint32 segment_id; /* offset 8; 4 bytes */
					   /* Followed by char page_image[BLCKSZ] (segment header block 0). */
} xl_cluster_undo_segment_init;


/*
 * Public API.
 */

/*
 * cluster_undo_emit_segment_init
 *	  Backend: emit a XLOG_UNDO_SEGMENT_INIT WAL record carrying the
 *	  full 8 KB segment header page image.  Caller owns page image
 *	  buffer (typically just-written via PageInitUndoSegmentHeader).
 *	  Returns the resulting LSN (caller may use for fsync / dirty
 *	  buffer LSN tracking).
 */
extern XLogRecPtr cluster_undo_emit_segment_init(uint8 instance, uint32 segment_id,
												 const char *page_image);

/*
 * cluster_undo_redo
 *	  RM redo handler entry point.  Dispatches by xl_info.
 */
extern void cluster_undo_redo(XLogReaderState *record);

/*
 * cluster_undo_desc
 *	  RM record description for pg_waldump / xlog dumps.
 */
extern void cluster_undo_desc(StringInfo buf, XLogReaderState *record);

/*
 * cluster_undo_identify
 *	  RM info code -> human-readable subtype name.  Returns NULL for
 *	  unknown subtypes.
 */
extern const char *cluster_undo_identify(uint8 info);


#endif /* CLUSTER_UNDO_XLOG_H */
