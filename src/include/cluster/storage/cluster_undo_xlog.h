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

#include "cluster/cluster_scn.h" /* SCN typedef (spec-3.11 D3 xl_undo_tt_slot_commit) */


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
#define XLOG_UNDO_TT_SLOT_COMMIT 0x30 /* spec-3.11 D3: durable TT slot commit_scn */


/*
 * On-disk WAL payload layout for XLOG_UNDO_SEGMENT_INIT.
 *
 *   payload = 12-byte fixed header + BLCKSZ-byte page image.
 *   Stored sequentially in a single XLogRegisterData block.
 *
 *   v0.2 P1-A: payload uses a custom path-based identification
 *   (instance + segment_id) rather than RelFileLocator/BlockNumber
 *   because pg_undo/ files live outside PG's relfilenode namespace.
 *   redo handler resolves the path via cluster_undo_path_resolve()
 *   and pwrites directly.
 *
 *   Hardening v1.0.4 P2-1: header is 12 bytes (not the "6-byte"
 *   v1.0.3 doc claimed); _pad[1] + _pad2[2] add up to 5 padding bytes
 *   that round segment_id to its uint32 8-byte alignment slot
 *   (1 + 1 + 4 + 4 = 10 bytes data; layout pads to offset 8 for
 *   segment_id, total struct size 12).  StaticAssertDecl below locks
 *   the on-disk WAL ABI; future maintainers see the assert fire if
 *   a field add / reorder breaks cross-version replay.
 */
typedef struct xl_cluster_undo_segment_init {
	uint8 instance;	   /* offset 0; 1 byte; owner instance (1..128); spec-1.21 Q5 */
	uint8 _pad[1];	   /* offset 1; 1 byte; alignment pad */
	uint16 _pad2[2];   /* offset 2; 4 bytes; pad up to uint32 alignment */
	uint32 segment_id; /* offset 8; 4 bytes */
					   /* Followed by char page_image[BLCKSZ] (segment header block 0). */
} xl_cluster_undo_segment_init;

/*
 * WAL ABI invariants (Hardening v1.0.4 P2-1; lessons SSOT L45 ext).
 *
 *   StaticAssertDecl locks struct size + segment_id offset so cross-
 *   version replay (mixed 1.22 / 1.23+ binaries) catches incompatible
 *   field add / reorder at compile time.
 */
StaticAssertDecl(sizeof(xl_cluster_undo_segment_init) == 12,
				 "xl_cluster_undo_segment_init must be exactly 12 bytes "
				 "(WAL ABI lock; Hardening v1.0.4 P2-1)");
StaticAssertDecl(offsetof(xl_cluster_undo_segment_init, segment_id) == 8,
				 "xl_cluster_undo_segment_init.segment_id must be at offset 8 "
				 "(uint32 8-byte alignment slot; Hardening v1.0.4 P2-1)");


/*
 * On-disk WAL payload for XLOG_UNDO_TT_SLOT_COMMIT (spec-3.11 D3).
 *
 *   24-byte fixed delta record (NO page image): identifies one TTSlot in the
 *   undo segment header block 0 and the commit_scn to durably stamp.  Unlike
 *   XLOG_UNDO_SEGMENT_INIT (full page image), this is a delta -- redo does a
 *   block-0 read-modify-write of the single slot, gated by the wrap-comparison
 *   table (spec-3.11 §2.3): rec.wrap > slot.wrap overwrites (recycle-then-commit,
 *   the normal path because BIND is not WAL-logged -- Q1 commit-only); rec.wrap
 *   == slot.wrap && same xid is idempotent; rec.wrap == slot.wrap && different
 *   xid is corruption (PANIC); rec.wrap < slot.wrap is stale (skip).
 *
 *   `instance` is carried in-record (like xl_cluster_undo_segment_init) so the
 *   redo handler resolves pg_undo/instance_<N>/seg_<id>.dat without depending on
 *   cluster_node_id (path-based, outside RelFileLocator namespace).
 *
 *   spec-3.11 v0.2.1 §2.3 carried no instance field; coding (step 1) found redo
 *   path resolution requires it -- folded into the 24B layout's pad slot (规则 10
 *   sync: spec §2.3 to amend at step-7 review).
 */
typedef struct xl_undo_tt_slot_commit {
	uint32 segment_id;	  /* offset 0;  4 B */
	uint16 slot_offset;	  /* offset 4;  2 B; [0, TT_SLOTS_PER_SEGMENT-1] raw (L48) */
	uint16 wrap;		  /* offset 6;  2 B; reuse generation */
	TransactionId xid;	  /* offset 8;  4 B; slot owner xid */
	uint8 instance;		  /* offset 12; 1 B; owner instance (1..128) */
	uint8 _pad[3];		  /* offset 13; 3 B; pad up to SCN 8-byte alignment */
	SCN commit_scn;		  /* offset 16; 8 B */
} xl_undo_tt_slot_commit; /* total 24 B */

StaticAssertDecl(sizeof(xl_undo_tt_slot_commit) == 24,
				 "xl_undo_tt_slot_commit must be exactly 24 bytes (WAL ABI lock; "
				 "spec-3.11 §2.3 + L45/L48)");
StaticAssertDecl(offsetof(xl_undo_tt_slot_commit, commit_scn) == 16,
				 "xl_undo_tt_slot_commit.commit_scn must be at offset 16 "
				 "(SCN 8-byte alignment slot; spec-3.11 L45)");


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
 * cluster_undo_emit_tt_slot_commit (spec-3.11 D3)
 *	  Backend: emit XLOG_UNDO_TT_SLOT_COMMIT for a durable commit_scn stamp on
 *	  one TT slot.  Caller emits this BEFORE the transaction's commit XLOG
 *	  record (spec-3.11 C1); the commit record's XLogFlush / group commit makes
 *	  it durable -- no independent fsync (spec-3.11 C10).  Returns the LSN.
 */
extern XLogRecPtr cluster_undo_emit_tt_slot_commit(uint8 instance, uint32 segment_id,
												   uint16 slot_offset, uint16 wrap,
												   TransactionId xid, SCN commit_scn);

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
