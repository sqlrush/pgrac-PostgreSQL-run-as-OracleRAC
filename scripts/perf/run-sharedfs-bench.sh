#!/bin/bash
#-------------------------------------------------------------------------
#
# run-sharedfs-bench.sh -- spec-4.5a D14 report-only perf surfaces.
#
# Two measurements, both REPORT-ONLY (rule 20.A: perf workflows are
# warn-only artifacts, not ship gates):
#
#   1. shared-I/O passthrough: single-node pgbench TPS with
#      cluster.smgr_user_relations=on, backend local vs cluster_fs
#      (same host, shared root on the same filesystem -- isolates the
#      sharedfs vtable + path-resolution overhead, not network).
#
#   2. merged-recovery throughput: wall-clock crash recovery of the
#      same row volume, single-stream (one node wrote everything)
#      vs k=2 merged (two nodes wrote half each, survivor merges).
#      Reported as seconds + rows/s.
#
# Manual / local invocation only (CI runners are too noisy for perf).
#
# Author: SqlRush <sqlrush@gmail.com>
# Portions Copyright (c) 2026, pgrac contributors
#
# Spec: spec-4.5a-shared-storage-data-backend.md (D14)
#
# IDENTIFICATION
#    scripts/perf/run-sharedfs-bench.sh
#
#-------------------------------------------------------------------------
set -euo pipefail

INSTALL="${PGRAC_ENABLE_INSTALL:-$HOME/linkdb-install}"
SCALE="${SCALE:-10}"
DURATION="${DURATION:-30}"
CLIENTS="${CLIENTS:-4}"
JOBS="${JOBS:-2}"
ROWS="${ROWS:-200000}"
OUTDIR="$(cd "$(dirname "$0")" && pwd)/results"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$OUTDIR/sharedfs-bench-$STAMP.txt"
WORK="$(mktemp -d /tmp/pgrac-sharedfs-bench.XXXXXX)"

PATH="$INSTALL/bin:$PATH"
export PGHOST="$WORK"

mkdir -p "$OUTDIR"
echo "== spec-4.5a D14 sharedfs bench $STAMP ==" | tee "$OUT"
echo "install=$INSTALL scale=$SCALE duration=${DURATION}s clients=$CLIENTS rows=$ROWS" | tee -a "$OUT"

common_conf() {
    local pgdata="$1" port="$2"
    cat >> "$pgdata/postgresql.conf" <<EOF
port = $port
unix_socket_directories = '$WORK'
listen_addresses = ''
cluster.enabled = on
cluster.node_id = 0
cluster.allow_single_node = on
cluster.smgr_user_relations = on
autovacuum = off
shared_buffers = 256MB
EOF
}

# ---- 1. pgbench: local vs cluster_fs ---------------------------------
bench_one() {
    local backend="$1" port="$2"
    local pgdata="$WORK/pgdata_$backend"
    initdb -D "$pgdata" -A trust -N > /dev/null
    common_conf "$pgdata" "$port"
    echo "cluster.shared_storage_backend = $backend" >> "$pgdata/postgresql.conf"
    if [ "$backend" = "cluster_fs" ]; then
        mkdir -p "$WORK/shared_$backend"
        echo "cluster.shared_data_dir = '$WORK/shared_$backend'" >> "$pgdata/postgresql.conf"
    fi
    pg_ctl -D "$pgdata" -l "$WORK/log_$backend" -w start > /dev/null
    pgbench -p "$port" -i -s "$SCALE" postgres > /dev/null 2>&1
    local tps
    tps=$(pgbench -p "$port" -c "$CLIENTS" -j "$JOBS" -T "$DURATION" postgres 2>/dev/null \
          | awk '/tps =/ {print $3; exit}')
    pg_ctl -D "$pgdata" -m fast -w stop > /dev/null
    echo "$tps"
}

echo "-- 1. shared-I/O pgbench (single node, smgr_user_relations=on)" | tee -a "$OUT"
TPS_LOCAL=$(bench_one local 54441)
TPS_SHARED=$(bench_one cluster_fs 54442)
echo "backend=local      tps=$TPS_LOCAL"  | tee -a "$OUT"
echo "backend=cluster_fs tps=$TPS_SHARED" | tee -a "$OUT"
awk -v a="$TPS_LOCAL" -v b="$TPS_SHARED" \
    'BEGIN { if (a > 0) printf "cluster_fs/local = %.1f%%\n", 100*b/a }' | tee -a "$OUT"

# ---- 2. merged-recovery throughput ------------------------------------
# Single-stream: one node writes $ROWS rows, crashes, recovers alone.
# Merged k=2: handled by the t/248-style harness; here we time the
# single-stream reference so the TAP-run merge time has a baseline.
echo "-- 2. crash-recovery reference (single stream, $ROWS rows)" | tee -a "$OUT"
PGDATA1="$WORK/pgdata_rec"
initdb -D "$PGDATA1" -A trust -N > /dev/null
common_conf "$PGDATA1" 54443
echo "cluster.shared_storage_backend = cluster_fs" >> "$PGDATA1/postgresql.conf"
mkdir -p "$WORK/shared_rec"
echo "cluster.shared_data_dir = '$WORK/shared_rec'" >> "$PGDATA1/postgresql.conf"
pg_ctl -D "$PGDATA1" -l "$WORK/log_rec" -w start > /dev/null
psql -p 54443 -q postgres -c "CREATE TABLE r (v int)" \
     -c "INSERT INTO r SELECT generate_series(1, $ROWS)"
# crash without checkpoint: the whole insert replays from WAL
pg_ctl -D "$PGDATA1" -m immediate -w stop > /dev/null
T0=$(date +%s)
pg_ctl -D "$PGDATA1" -l "$WORK/log_rec" -w start > /dev/null
T1=$(date +%s)
psql -p 54443 -Atq postgres -c "SELECT count(*) FROM r" | tee -a "$OUT"
pg_ctl -D "$PGDATA1" -m fast -w stop > /dev/null
echo "single-stream recovery: $((T1 - T0))s for $ROWS rows" | tee -a "$OUT"

echo "results: $OUT"
rm -rf "$WORK"
