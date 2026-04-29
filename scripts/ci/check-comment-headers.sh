#!/bin/bash
#-------------------------------------------------------------------------
#
# check-comment-headers.sh
#    CI helper: enforce pgrac comment-header conventions on changed
#    files (CLAUDE.md rule 11).
#
#    Run from repository root.  Compares HEAD against the merge base
#    with origin/main (or against origin/main directly on push events).
#
# IDENTIFICATION
#    scripts/ci/check-comment-headers.sh
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
# NOTES
#    Checks performed:
#    1. Newly-added cluster source files (src/(backend|include)/cluster
#       /**.{c,h}) must contain "Author: SqlRush <sqlrush@gmail.com>".
#    2. Newly-added cluster test files (src/test/cluster_(unit|tap|
#       regress)/**.{c,h,pl,pm}) must contain the same Author line.
#    3. Modified PG-original files (anywhere under src/ but NOT
#       under cluster_*/cluster/ directories) should contain a "PGRAC"
#       marker if the diff is non-trivial.  Stage 0.6: warn-only.
#       Stage 0.7+: tighten to error.
#
#    Exits non-zero on hard failures (check #1, #2).
#
#-------------------------------------------------------------------------

set -e
set -o pipefail

# ----------
# Determine the diff range
# ----------
# On push to main: compare HEAD against the previous commit on origin/main.
# On PR: GitHub Actions exposes pull_request.base.sha via event payload,
#   but the simpler portable approach is to diff against origin/main.
# Locally: diff against origin/main as a default.

if [ -n "$GITHUB_BASE_REF" ]; then
    # PR event: compare against PR base
    git fetch --no-tags --prune origin "$GITHUB_BASE_REF" >/dev/null 2>&1 || true
    BASE="origin/$GITHUB_BASE_REF"
elif git rev-parse --verify origin/main >/dev/null 2>&1; then
    # push event or local: compare against origin/main parent commit
    BASE=$(git merge-base origin/main HEAD || echo origin/main)
else
    # First push (no origin/main yet): compare against HEAD~1 if available
    BASE=$(git rev-parse HEAD~1 2>/dev/null || echo HEAD)
fi

echo "# check-comment-headers.sh: comparing against $BASE"

errors=0
warnings=0


# ----------
# Check 1+2: newly-added cluster files must carry Author
# ----------
new_files=$(git diff --diff-filter=A --name-only "$BASE" HEAD || true)

for f in $new_files; do
    case "$f" in
        src/backend/cluster/*.c | \
        src/backend/cluster/*.h | \
        src/include/cluster/*.c | \
        src/include/cluster/*.h | \
        src/test/cluster_unit/*.c | \
        src/test/cluster_unit/*.h | \
        src/test/cluster_unit/*.pl | \
        src/test/cluster_unit/*.pm | \
        src/test/cluster_tap/*.pl | \
        src/test/cluster_tap/*.pm | \
        src/test/cluster_tap/lib/*.pm | \
        src/test/cluster_tap/t/*.pl | \
        src/test/cluster_regress/*)
            if ! grep -q 'Author: SqlRush' "$f" 2>/dev/null; then
                echo "::error file=$f::Missing 'Author: SqlRush <sqlrush@gmail.com>' header (CLAUDE.md rule 11)"
                errors=$((errors + 1))
            fi
            ;;
    esac
done


# ----------
# Check 3: modified PG-original files must have PGRAC marker
# ----------
# Stage 0.7 tightening (per spec-0.6 Q5 commitment): missing PGRAC
# marker on a modified PG file is now a HARD ERROR (was warning in
# stage 0.6).
#
# Rationale: CLAUDE.md rule 11 requires every diverging change to a
# PG-original file to be visibly tagged so the codebase remains
# easily auditable for upstream alignment / vendor patches.
modified_files=$(git diff --diff-filter=M --name-only "$BASE" HEAD || true)

for f in $modified_files; do
    case "$f" in
        # Skip pgrac-owned directories (their own files don't need PGRAC marker)
        src/backend/cluster/* | src/include/cluster/* | \
        src/test/cluster_unit/* | src/test/cluster_tap/* | \
        src/test/cluster_regress/*)
            continue
            ;;
        # Modified files inside PG source tree
        src/*)
            if ! grep -q 'PGRAC' "$f" 2>/dev/null; then
                echo "::error file=$f::Modified PG file lacks any 'PGRAC' marker (CLAUDE.md rule 11). Add a PGRAC MODIFICATIONS section in the file header or a /* PGRAC: ... */ inline tag at each change site."
                errors=$((errors + 1))
            fi
            ;;
    esac
done


# ----------
# Summary
# ----------
echo ""
echo "# check-comment-headers.sh: summary"
echo "#   errors:   $errors"
echo "#   warnings: $warnings"

if [ "$errors" -gt 0 ]; then
    echo "# FAILED: $errors hard error(s)"
    exit 1
fi

echo "# OK"
exit 0
