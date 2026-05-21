#!/bin/bash
#-------------------------------------------------------------------------
#
# check-no-clog-overlay.sh
#    CI lint: ban the v0.1 CLOG-overlay foundation path (spec-3.1 L176).
#
#    spec-3.1 v0.1 erroneously proposed a CLOG-overlay-as-foundation
#    design (`cluster_clog_overlay`, `SharedInvalCLOGStatusMsg`,
#    `PGRAC_IC_MSG_TT_STATUS_HINT`).  That direction is permanently
#    rejected (AD-006 第五轮 / feature-069 lock).  This lint enforces
#    the lesson by failing CI if any of the rejected identifiers leak
#    into linkdb source code.
#
# IDENTIFICATION
#    scripts/ci/check-no-clog-overlay.sh
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
# NOTES
#    Strategy: grep -rnE the three banned identifiers under src/.
#    Allowed contexts:
#      - any line carrying the marker `/* SPEC_3_1_LINT_OK: */`
#        (e.g. a future hardening spec that intentionally references
#        the rejected name in a removal comment).
#
#    Anchors L176 in code, complementing the docs/spec-drafting-lessons
#    L176 entry and feature-069 §"AD-006 第五轮" guardrail.
#
#-------------------------------------------------------------------------

set -euo pipefail

BANNED_RE='cluster_clog_overlay|SharedInvalCLOGStatusMsg|PGRAC_IC_MSG_TT_STATUS_HINT'

# Use git ls-files so we only scan tracked files; bypasses build artifacts
# and untracked log directories.
matches=$(git ls-files 'src/*' 2>/dev/null \
	| xargs grep -nE "$BANNED_RE" 2>/dev/null \
	| grep -v 'SPEC_3_1_LINT_OK' \
	|| true)

if [ -n "$matches" ]; then
	echo "ERROR: spec-3.1 L176 lint failed — banned CLOG-overlay identifiers found:" >&2
	echo "$matches" >&2
	echo "" >&2
	echo "spec-3.1 v0.1 -> v1.0 hard-redirected the foundation from" >&2
	echo "CLOG-overlay to ITL/TT exact-key (AD-006 第五轮 / feature-069)." >&2
	echo "These names are permanently rejected." >&2
	echo "See: specs/spec-3.1-cluster-xid-status-foundation.md §0.1" >&2
	echo "     docs/spec-drafting-lessons.md L176" >&2
	exit 1
fi

echo "spec-3.1 L176 lint passed (no CLOG-overlay identifiers)."
