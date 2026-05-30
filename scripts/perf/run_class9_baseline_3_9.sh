#!/bin/bash
# spec-3.9 D12 — perf class 9 smoke baseline: CR construction entry pressure.
#
#   bash + psql + timestamp loop (no ClusterPair perl fixture, per spec-3.9
#   §1.2 D12 to avoid the spec-3.8 fixture hang).  Single-node smoke:
#   measures the CR construction entry-path cost + verifies the CR code is
#   genuinely reached (counter deltas).  Honest scope: single node has no
#   ITL pages, so constructs take the early fail-closed path; full 2-node
#   construct-success TPS is deferred with the MVCC-gate codereview.
#
# Author: SqlRush <sqlrush@gmail.com>
set -euo pipefail

INSTALL="${PGRAC_ENABLE_INSTALL:-/tmp/pgrac-install}"
BIN="$INSTALL/bin"
DATA=/tmp/pgrac-class9-data
PORT="${PGPORT:-54390}"
DUR="${DURATION:-15}"

rm -rf "$DATA"
"$BIN/initdb" -D "$DATA" --auth=trust >/dev/null 2>&1
echo "port = $PORT" >> "$DATA/postgresql.conf"
echo "logging_collector = off" >> "$DATA/postgresql.conf"
"$BIN/pg_ctl" -D "$DATA" -l /tmp/pgrac-class9.log -w start >/dev/null

psql() { "$BIN/psql" -p "$PORT" -d postgres -X "$@"; }

psql -c "CREATE TABLE t_cr_perf (id int, v text); INSERT INTO t_cr_perf VALUES (1,'a');" >/dev/null

cr_counter() { psql -tAc "SELECT value FROM pg_cluster_state WHERE category='cr' AND key='$1';"; }

pre_construct=$(cr_counter cr_construct_count)
pre_corrupt=$(cr_counter cr_corruption_count)

start=$(date +%s)
count=0
while [ $(( $(date +%s) - start )) -lt "$DUR" ]; do
    psql -c "SELECT cluster_cr_test_construct('t_cr_perf'::regclass,0,0,9223372036854775807);" >/dev/null 2>&1 || true
    count=$((count+1))
done
elapsed=$(( $(date +%s) - start ))
tps=$(echo "scale=1; $count / $elapsed" | bc)

post_construct=$(cr_counter cr_construct_count)
post_corrupt=$(cr_counter cr_corruption_count)

echo "RESULT class9 cr-construction-entry-pressure (single-node smoke):"
echo "  calls=$count elapsed=${elapsed}s tps=$tps"
echo "  cr_construct_count delta = $((post_construct - pre_construct))"
echo "  cr_corruption_count delta = $((post_corrupt - pre_corrupt))  (no-ITL fail-closed path on single node)"
echo "  chain_walk_steps_sum = $(cr_counter cr_chain_walk_steps_sum)"

"$BIN/pg_ctl" -D "$DATA" -m immediate stop >/dev/null 2>&1 || true
