#!/bin/bash
#-------------------------------------------------------------------------
#
# check-tidy.sh
#    CI helper: run clang-tidy on pgrac cluster sources (warn-only).
#
#    Stage 0.7: warn-only -- always exits 0 even if warnings found.
#    Output is captured for human review.
#
#    Stage 0.27 (deep static analysis) will tighten this to fail on
#    new warnings introduced by a change.
#
# IDENTIFICATION
#    scripts/ci/check-tidy.sh
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
#-------------------------------------------------------------------------

set -e
set -o pipefail

# Locate clang-tidy
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    for v in 18 17 16 15 14; do
        if command -v "clang-tidy-$v" >/dev/null 2>&1; then
            CLANG_TIDY="clang-tidy-$v"
            break
        fi
    done
fi

if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    echo "# check-tidy.sh: clang-tidy not found, skipping (stage 0.7 warn-only)"
    exit 0
fi

echo "# check-tidy.sh: using $($CLANG_TIDY --version | head -1)"

# Cluster paths
PATHS=(
    "src/backend/cluster"
    "src/test/cluster_unit"
)

# Include path for cluster headers
INCLUDE_FLAGS="-Isrc/include -Isrc/test/cluster_unit"

warnings=0
checked=0

for dir in "${PATHS[@]}"; do
    if [ ! -d "$dir" ]; then
        continue
    fi
    while IFS= read -r f; do
        checked=$((checked + 1))
        # Run clang-tidy; do not fail on warnings (stage 0.7).
        if output=$("$CLANG_TIDY" "$f" -- $INCLUDE_FLAGS 2>&1); then
            true
        fi
        # Count "warning:" lines, but don't fail.
        warn_count=$(echo "$output" | grep -c 'warning:' || true)
        if [ "$warn_count" -gt 0 ]; then
            echo "# clang-tidy warnings in $f: $warn_count"
            warnings=$((warnings + warn_count))
        fi
    done < <(find "$dir" -type f -name '*.c')
done

echo ""
echo "# check-tidy.sh: summary (stage 0.7 warn-only)"
echo "#   files checked: $checked"
echo "#   warnings:      $warnings"
echo "# OK (stage 0.27 will tighten to fail-on-new-warning)"
exit 0
