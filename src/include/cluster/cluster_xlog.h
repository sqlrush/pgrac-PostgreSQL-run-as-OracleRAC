/*-------------------------------------------------------------------------
 *
 * cluster_xlog.h
 *	  pgrac cluster WAL helpers (spec-1.19 page header thread_id placeholder).
 *
 *	  Stage 1 ships only the validator helper for the
 *	  XLogPageHeaderData::xlp_thread_id field defined in
 *	  access/xlog_internal.h.  The five sentinel constants
 *	  (XLP_THREAD_ID_LEGACY / FIRST_REAL / MAX_REAL / INVALID and
 *	  XLP_CLUSTER_FLAGS_RESERVED) live in xlog_internal.h so frontend
 *	  tools (pg_waldump / pg_resetwal) can read them without pulling in
 *	  cluster headers.  This header layers the validator on top.
 *
 *	  Stage 2+ feature-034 (per-instance redo thread) will activate
 *	  real thread IDs starting at XLP_THREAD_ID_FIRST_REAL = 1, mapping
 *	  thread_id = cluster_node_id + 1 so XLP_THREAD_ID_LEGACY (0) stays
 *	  permanently reserved as the sentinel.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, pgrac contributors
 *
 * Author: SqlRush <sqlrush@gmail.com>
 *
 * IDENTIFICATION
 *	  src/include/cluster/cluster_xlog.h
 *
 * NOTES
 *	  This is a pgrac-original file (no derivation from PostgreSQL).
 *	  Spec: spec-1.19-wal-page-header-thread-id.md APPROVED 2026-05-05 v0.2
 *	  Design: docs/wal-record-format-design.md §5.1
 *	  AD-009 (Per-instance redo thread + 共享存储 + merged recovery)
 *
 *	  Frontend-safe: this header has no backend-only includes.  pg_waldump
 *	  consumes XLogReaderGetThreadId() from access/xlogreader.h directly
 *	  and does not need this file; this file is for backend recovery /
 *	  apply paths in spec-1.21+.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_XLOG_H
#define CLUSTER_XLOG_H

#include "c.h" /* uint16, bool */

/* Sentinel constants are defined in access/xlog_internal.h alongside the
 * XLogPageHeaderData struct so frontend code (pg_waldump / pg_resetwal)
 * can use them without pulling in cluster headers.  We re-export only the
 * helper here. */


/*
 * cluster_xlog_validate_page_header_thread_id -- Stage 1 thread_id check.
 *
 *	Returns true iff `tid` equals the legacy sentinel value (= 0).
 *	Convenience wrapper for direct callers (unit tests, ad-hoc
 *	verification scripts) that want to validate a thread_id field in
 *	isolation; the full Stage 1 invariant (thread_id AND cluster_flags)
 *	is exposed via cluster_xlog_validate_page_header_stage1_invariant
 *	below.
 *
 *	Stage 2+ feature-034 will replace the body with a real range check
 *	XLP_THREAD_ID_FIRST_REAL <= tid <= XLP_THREAD_ID_MAX_REAL.  The
 *	function name is stable across stages so callers do not break.
 *
 *	Implemented as `static inline` so frontend tools (pg_waldump /
 *	pg_resetwal) and unit tests can link it without an extra
 *	cluster_xlog.o dependency.  Pulls in access/xlog_internal.h for the
 *	XLP_THREAD_ID_LEGACY constant.
 */
#include "access/xlog_internal.h"

static inline bool
cluster_xlog_validate_page_header_thread_id(uint16 tid)
{
	return tid == XLP_THREAD_ID_LEGACY;
}


/*
 * cluster_xlog_validate_page_header_stage1_invariant -- full Stage 1 check.
 *
 *	Returns true iff (thread_id == LEGACY && cluster_flags == RESERVED).
 *	This is the SAME predicate enforced by XLogReaderValidatePageHeader
 *	at xlogreader.c — exposing it as a testable helper here so cluster_unit
 *	(no PG state) can verify the predicate in isolation; the reader
 *	hook calls this helper to keep logic in one place.
 *
 *	Stage 2+ feature-034 / future spec will replace the body with a real
 *	range check (thread_id in [FIRST_REAL..MAX_REAL]; cluster_flags
 *	mask of valid bits).  Function name is stable across stages so the
 *	xlogreader hook + cluster_unit tests track the evolving predicate
 *	without churn.
 *
 *	Hardening v1.0.1 P2-2 (codex review 2026-05-05): added so the
 *	negative-path xlogreader rejection logic has unit-test coverage.
 *	Previously only the inline thread_id helper had tests; the
 *	xlogreader hook itself had zero tests, meaning an accidental delete
 *	or condition flip would not be caught by CI.
 */
static inline bool
cluster_xlog_validate_page_header_stage1_invariant(uint16 thread_id, uint16 cluster_flags)
{
	return thread_id == XLP_THREAD_ID_LEGACY && cluster_flags == XLP_CLUSTER_FLAGS_RESERVED;
}


#endif /* CLUSTER_XLOG_H */
