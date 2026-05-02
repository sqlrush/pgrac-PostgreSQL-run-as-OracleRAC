/*-------------------------------------------------------------------------
 *
 * cluster_scn.h
 *	  pgrac cluster SCN (System Change Number) typedef + invariants.
 *
 *	  Stage 1.4 ships only the typedef + InvalidScn constant + SCN_VALID
 *	  / SCN_FORMAT macros.  The full encoding layer (scn_encode /
 *	  scn_decode / SCN_NODE_ID_BITS / SCN_LOCAL_BITS) and the comparison
 *	  contract (scn_time_cmp / scn_total_cmp / scn_recovery_cmp + grep
 *	  gate) land at spec-1.15 when local_scn maintenance starts (refer
 *	  to docs/scn-protocol-design.md §3.2 + §3.2.1 v1.1).
 *
 *	  On disk: pd_block_scn (PageHeaderData, stage 1.4), commit_scn
 *	  (ITL slot, stage 1.5), write_scn (ITL slot, stage 1.5), xl_scn
 *	  (WAL record, stage 1.18), and TT slot fields (undo segment,
 *	  stage 1.20) all use this typedef.
 *
 *	  InvalidScn locked to 0 (spec-1.4 §8 Q2 = A) -- matches PG's
 *	  InvalidTransactionId convention; lets MemSet zero-init occupy the
 *	  "not yet set" semantics naturally.  SCN protocol guarantees all
 *	  real values are >= 1 (monotonically advancing per local node), so
 *	  0 is permanently reserved as the "absent" sentinel.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_scn.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-1.4-block-format-pageheader-scn.md
 *	  Design: docs/scn-protocol-design.md v1.1 §3.2 + §3.2.1
 *	  AD-008 (SCN protocol; Lamport-style distributed counters).
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_SCN_H
#define CLUSTER_SCN_H

#include "c.h" /* uint64 */


/*
 * SCN -- System Change Number.
 *
 *	Stage 1.4 stub: 8-byte unsigned integer.  Stage 1.15 will overlay
 *	the encoding layer (8b node_id || 56b local_scn) on top of this
 *	typedef; the stub already reserves the byte width so that no on-
 *	disk format change is needed when the encoding layer lands.
 */
typedef uint64 SCN;


/*
 * InvalidScn -- the "absent" / "not yet set" sentinel.
 *
 *	Locked to 0 by spec-1.4 §8 Q2 = A.  Real SCN values always >= 1
 *	(monotonically advancing per local node, per AD-008).
 */
#define InvalidScn ((SCN)0)


/*
 * SCN_VALID -- hot-path validity check.
 *
 *	Prefer this macro over direct comparison so future encoding-layer
 *	changes (spec-1.15+) can centralize semantics here.  In particular,
 *	spec-1.15 will introduce scn_time_cmp / scn_total_cmp /
 *	scn_recovery_cmp comparison functions; business code MUST NOT use
 *	`a < b` / `a == b` / `a > b` on SCN values (CI grep gate enforces;
 *	exception: `scn != InvalidScn` via this macro).
 *
 *	See docs/scn-protocol-design.md §3.2.1.
 */
#define SCN_VALID(scn) ((scn) != InvalidScn)


/*
 * SCN_FORMAT / SCN_FORMAT_ARG -- printf-style format helpers.
 *
 *	Use the (unsigned long) cast in SCN_FORMAT_ARG for portability:
 *	uint64 prints as %lu on 64-bit Linux/macOS and as %llu on 32-bit
 *	platforms; (unsigned long) defers the choice to the caller's ABI
 *	while guaranteeing %lu is always correct.
 */
#define SCN_FORMAT "%lu"
#define SCN_FORMAT_ARG(scn) ((unsigned long)(scn))


#endif /* CLUSTER_SCN_H */
