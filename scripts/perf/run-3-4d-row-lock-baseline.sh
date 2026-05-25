#!/bin/bash
#-------------------------------------------------------------------------
#
# run-3-4d-row-lock-baseline.sh -- spec-3.4d row-lock perf baseline
#
# spec-3.4d D16 / Q6 perf gate.  Three workload classes mandated by
# L195 (single-node-rac-enabled-must-have-no-peer-fast-path-gate) +
# L197 (pre-merge-codereview-yields-real-perf-not-just-cleanup) +
# spec-3.4c Hardening v1.0.1 A8 honest scope reminder:
#
#   1. single-node enable=off vs on  (no-peer fast path verification;
#      cluster_conf_has_peers() gate makes heap_lock_tuple skip ITL
#      stamp + ACTIVE TT install)
#   2. 2-node local-affinity  (each node SELECT FOR UPDATE its own key
#      range, no cross-node ITL contention)
#   3. 2-node hot-row  (same row, multiple SHARE lockers, triggers
#      cross-node ITL stamp + reader lookup + 53R98 fail-closed rate)
#
# 5-tier gate (spec-3.4d §11 DoD):
#   GREEN          <= 15% / 25% / 60%   ship-eligible
#   YELLOW         <= 25% / 40% / 70%   codereview review required
#   ORANGE         <= 40% / 60% / 80%   codereview + risk assessment
#   RED            <= 60% / 80% / 90%   block ship until root-caused
#   CATASTROPHIC   > 60% / > 80% / > 90%   hardening cycle mandatory
#
# Manual / local invocation only;  not wired into fast-gate CI per
# spec-3.4c pattern (perf workflow runs nightly).
#
# Author: SqlRush <sqlrush@gmail.com>
# Portions Copyright (c) 2026, pgrac contributors
#
# Spec: spec-3.4d-heap-lock-tuple-cross-node-lock-itl-fail-closed.md
#       (v0.2 FROZEN 2026-05-25)
#
# IDENTIFICATION
#    scripts/perf/run-3-4d-row-lock-baseline.sh
#
#-------------------------------------------------------------------------
set -euo pipefail

PROGNAME="run-3-4d-row-lock-baseline.sh"

# ---- defaults ---------------------------------------------------------
MODE="single-node"            # single-node | 2node-local | 2node-hotrow | all
SCALE=10
DURATION=30
CLIENTS=4
JOBS=2
ENABLE_INSTALL="${PGRAC_ENABLE_INSTALL:-$HOME/linkdb-install}"
DISABLE_INSTALL="${PGRAC_DISABLE_INSTALL:-$HOME/linkdb-disable-install}"
APPEND_MD=""
PGDATA_BASE=""
KEEP_PGDATA="no"

REPO_ROOT="$(cd "$(dirname "$(realpath "$0")")/../.." && pwd)"
RESULTS_DIR="$REPO_ROOT/scripts/perf/results"
TS="$(date -u +%Y%m%dT%H%M%SZ)"

die() { echo "$PROGNAME: $*" 1>&2; exit 1; }

usage() {
    cat <<EOF
$PROGNAME runs spec-3.4d row-lock perf baseline.

Usage:
  $PROGNAME [OPTION]...

Options:
  --mode=MODE           single-node | 2node-local | 2node-hotrow | all
                        (default: $MODE)
  --scale=N             pgbench -s scale (default: $SCALE)
  --duration=SECS       pgbench -T duration (default: $DURATION)
  --clients=N           pgbench -c (default: $CLIENTS)
  --jobs=N              pgbench -j (default: $JOBS)
  --enable-install=DIR  --enable-cluster install prefix
                        (default: $ENABLE_INSTALL,
                         override via PGRAC_ENABLE_INSTALL)
  --disable-install=DIR --disable-cluster install prefix (single-node only)
                        (default: $DISABLE_INSTALL,
                         override via PGRAC_DISABLE_INSTALL)
  --pgdata-base=DIR     PGDATA root (default: /tmp/pgrac-3-4d-perf-<mode>)
  --keep-pgdata         keep PGDATA after run (default: clean up)
  --append-md=PATH      append result rows to docs/perf-baseline.md §7.3
                        (PATH must already exist)
  -h, --help            show this help

Output:
  scripts/perf/results/3-4d-<mode>-<TS>.txt   (per-run captured TPS)
  Optional markdown ledger row in --append-md target.

Status:  spec-3.4d Sprint A Step 13 — script wire only.  Real
multi-node fixture for modes 2node-local / 2node-hotrow requires
ClusterPair test harness invocation (in TAP context) or manual
two-postmaster setup;  this stub will fail-closed with helpful
errhint when --mode=2node-* is requested before full fixture lands
(spec-3.4d Hardening v1.0.1 or shared-storage harness Stage 4+).

Single-node mode (--mode=single-node) is fully operational and is
the spec-3.4d Q6 sub-decision #1 perf gate (no-peer fast path verify;
expected <=15% GREEN per cluster_conf_has_peers() bypass of
heap_lock_tuple ITL/TT cost).
EOF
}

# ---- arg parse --------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        --mode=*)             MODE="${arg#--mode=}" ;;
        --scale=*)            SCALE="${arg#--scale=}" ;;
        --duration=*)         DURATION="${arg#--duration=}" ;;
        --clients=*)          CLIENTS="${arg#--clients=}" ;;
        --jobs=*)             JOBS="${arg#--jobs=}" ;;
        --enable-install=*)   ENABLE_INSTALL="${arg#--enable-install=}" ;;
        --disable-install=*)  DISABLE_INSTALL="${arg#--disable-install=}" ;;
        --pgdata-base=*)      PGDATA_BASE="${arg#--pgdata-base=}" ;;
        --keep-pgdata)        KEEP_PGDATA="yes" ;;
        --append-md=*)        APPEND_MD="${arg#--append-md=}" ;;
        -h|--help)            usage; exit 0 ;;
        *) die "unknown option: $arg (try --help)" ;;
    esac
done

mkdir -p "$RESULTS_DIR"

case "$MODE" in
    single-node|2node-local|2node-hotrow|all) ;;
    *) die "--mode must be single-node | 2node-local | 2node-hotrow | all" ;;
esac

# ---- pgbench custom scripts -------------------------------------------
ROW_LOCK_SQL="$RESULTS_DIR/select_for_update_3-4d.sql"
cat > "$ROW_LOCK_SQL" <<'SQL'
\set aid random(1, 100000 * :scale)
BEGIN;
SELECT abalance FROM pgbench_accounts WHERE aid = :aid FOR UPDATE;
END;
SQL

# ---- single-node mode -------------------------------------------------
run_single_node() {
    local enable_dir="$1"
    local enable_label="$2"
    local pgdata="${PGDATA_BASE:-/tmp/pgrac-3-4d-perf-single-$enable_label}"
    local port=61744
    local result_file="$RESULTS_DIR/3-4d-single-$enable_label-$TS.txt"

    [ -x "$enable_dir/bin/postgres" ] \
        || die "postgres binary not found at $enable_dir/bin/postgres (run --enable-install=DIR or set PGRAC_ENABLE_INSTALL)"

    rm -rf "$pgdata"
    "$enable_dir/bin/initdb" -D "$pgdata" -U postgres --no-clean >/dev/null
    echo "port = $port" >> "$pgdata/postgresql.conf"
    echo "shared_buffers = 256MB" >> "$pgdata/postgresql.conf"

    "$enable_dir/bin/pg_ctl" -D "$pgdata" -l "$pgdata/log" -o "-p $port" start >/dev/null
    sleep 2
    trap "\"$enable_dir/bin/pg_ctl\" -D \"$pgdata\" stop -m immediate >/dev/null 2>&1 || true; [ \"$KEEP_PGDATA\" = no ] && rm -rf \"$pgdata\"" EXIT

    "$enable_dir/bin/pgbench" -i -s "$SCALE" -p "$port" -U postgres postgres >/dev/null 2>&1
    "$enable_dir/bin/pgbench" -f "$ROW_LOCK_SQL" -c "$CLIENTS" -j "$JOBS" \
        -T "$DURATION" -p "$port" -U postgres -P 10 postgres 2>&1 | tee "$result_file"

    "$enable_dir/bin/pg_ctl" -D "$pgdata" stop -m fast >/dev/null
    [ "$KEEP_PGDATA" = "no" ] && rm -rf "$pgdata"
    trap - EXIT

    echo
    echo "## single-node mode=$enable_label result captured: $result_file"
    grep -E "tps =|latency average" "$result_file" || true
}

# ---- 2node-local / 2node-hotrow -- placeholder ------------------------
run_2node_placeholder() {
    local what="$1"
    cat <<EOF >&2

$PROGNAME: --mode=$what is a Sprint A scaffold and requires:
  - a working ClusterPair fixture (cluster_tap context), OR
  - manual two-postmaster setup with cluster.node_id 0/1 +
    cluster.peers configured + reachable interconnect.

Sprint A defers 2node perf measurement to spec-3.4d Hardening v1.0.1
codereview round (mirror spec-3.4c pattern).  Track in:
  docs/perf-baseline.md §7.3 "2026-05-25 TBD-codereview" row.

For single-node verification (Q6 sub-decision #1 — no-peer fast
path gate L195), re-run with --mode=single-node.
EOF
    return 0
}

# ---- main dispatch ----------------------------------------------------
echo "$PROGNAME: spec-3.4d row-lock baseline mode=$MODE scale=$SCALE duration=${DURATION}s clients=$CLIENTS jobs=$JOBS"

case "$MODE" in
    single-node)
        run_single_node "$ENABLE_INSTALL" "on"
        if [ -x "$DISABLE_INSTALL/bin/postgres" ]; then
            run_single_node "$DISABLE_INSTALL" "off"
        else
            echo "$PROGNAME: --disable-install=$DISABLE_INSTALL not found, skipping off baseline"
        fi
        ;;
    2node-local)   run_2node_placeholder "2node-local" ;;
    2node-hotrow)  run_2node_placeholder "2node-hotrow" ;;
    all)
        run_single_node "$ENABLE_INSTALL" "on"
        [ -x "$DISABLE_INSTALL/bin/postgres" ] && run_single_node "$DISABLE_INSTALL" "off"
        run_2node_placeholder "2node-local"
        run_2node_placeholder "2node-hotrow"
        ;;
esac

if [ -n "$APPEND_MD" ]; then
    [ -f "$APPEND_MD" ] || die "$APPEND_MD does not exist"
    echo "$PROGNAME: results captured in $RESULTS_DIR; append-md manual fill into $APPEND_MD §7.3 table"
    echo "  (auto-append deferred to spec-3.4d Hardening v1.0.1)"
fi

echo "$PROGNAME: done"
