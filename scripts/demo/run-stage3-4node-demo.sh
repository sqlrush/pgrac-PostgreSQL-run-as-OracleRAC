#!/usr/bin/env bash
#
# scripts/demo/run-stage3-4node-demo.sh
#	  spec-3.17 D4 — Stage 3 MVCC 4-node shared-storage technical demo
#	  (manual / pre-release only).
#
#	  Demonstrates the Stage 3 MVCC machinery across FOUR nodes that share
#	  storage, in sequence:
#	    1. cross-node INSERT/UPDATE visibility
#	    2. CR historical read (held snapshot survives a peer's commit)
#	    3. cross-node 2PC (PREPARE on one node, COMMIT PREPARED, visible on all)
#	    4. retention / undo cleaner recycle
#	    5. crash-restart recovery (one node) with the cluster intact
#
#	  IMPORTANT — this script does NOT provision a cluster.  pgrac currently
#	  ships a coordination layer + per-node local storage + a shared voting
#	  disk;  it has NO real shared-storage backend yet (cluster_shared_fs is
#	  stub + local only — NVMe-oF / multi-attach is a future Stage 6 / RDMA
#	  tier).  So a genuine 4-node shared-storage demonstration requires an
#	  EXTERNALLY provisioned, controlled 4-node shared-storage cluster, whose
#	  connection strings are passed in.  Absent that environment the script
#	  SKIPS with a clear reason — it never fakes a pass.
#
#	  NOT PRODUCTION-SAFE:  Stage 3 has no node fencing (Stage 4 / feature
#	  #125 watchdog/fencer plane).  Run only against a throwaway demo
#	  cluster.  See docs/development-roadmap.md "Release Implications".
#
#	  Connection strings (either form):
#	    --node0 CONNSTR ... --node3 CONNSTR
#	    STAGE3_NODE0_CONNSTR ... STAGE3_NODE3_CONNSTR  (env)
#
#	  Output:  tmp/stage3-4node-demo-<TIMESTAMP>.json  (correctness verdicts
#	  + honest baseline);  exit 0 = all correctness assertions passed,
#	  exit 64 = SKIP (no environment), exit 1 = a correctness assertion
#	  FAILED.
#
# Author: SqlRush <sqlrush@gmail.com>
# Portions Copyright (c) 2026, pgrac contributors
#

set -e
set -o pipefail

EXIT_SKIP=64

INSTALL_PREFIX="${INSTALL_PREFIX:-/tmp/pgrac-install}"
PSQL="${PSQL:-$INSTALL_PREFIX/bin/psql}"

N0="${STAGE3_NODE0_CONNSTR:-}"
N1="${STAGE3_NODE1_CONNSTR:-}"
N2="${STAGE3_NODE2_CONNSTR:-}"
N3="${STAGE3_NODE3_CONNSTR:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--node0) N0="$2"; shift 2 ;;
		--node1) N1="$2"; shift 2 ;;
		--node2) N2="$2"; shift 2 ;;
		--node3) N3="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,40p' "$0"; exit 0 ;;
		*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

TIMESTAMP="$(date '+%Y%m%dT%H%M%S')"
OUT_DIR="${OUT_DIR:-tmp}"
mkdir -p "$OUT_DIR"
OUT_JSON="$OUT_DIR/stage3-4node-demo-$TIMESTAMP.json"

# ----- SKIP (honest) when the external 4-node environment is absent -----
if [ -z "$N0" ] || [ -z "$N1" ] || [ -z "$N2" ] || [ -z "$N3" ]; then
	reason="no external 4-node shared-storage cluster provided (set --node0..3 or STAGE3_NODE0..3_CONNSTR); pgrac has no real shared-storage backend this cycle (cluster_shared_fs stub+local only) — real demonstration deferred to a shared-storage backend (Stage 6 / dedicated spec)"
	echo "SKIP: $reason"
	cat > "$OUT_JSON" <<EOF
{
  "spec": "3.17",
  "demo": "4node-shared-storage",
  "timestamp": "$TIMESTAMP",
  "status": "SKIP",
  "reason": "$reason",
  "results": []
}
EOF
	echo "wrote $OUT_JSON (status=SKIP)"
	exit "$EXIT_SKIP"
fi

[ -x "$PSQL" ] || { echo "psql not found at $PSQL" >&2; exit 3; }

echo "=== spec-3.17 D4 — Stage 3 MVCC 4-node shared-storage demo ==="
echo "*** NOT PRODUCTION-SAFE (no Stage 3 fencing) — throwaway demo cluster only ***"
echo "node0=$N0"
echo "node1=$N1"
echo "node2=$N2"
echo "node3=$N3"

RESULTS=()
FAILS=0

# q NODE SQL -> trimmed scalar result
q() { "$PSQL" -X -A -t -q -d "$1" -c "$2"; }

# assert NAME EXPECT ACTUAL
assert() {
	local name="$1" expect="$2" actual="$3" ok="true"
	if [ "$expect" != "$actual" ]; then ok="false"; FAILS=$((FAILS + 1)); fi
	printf '  [%s] %s (expect=%s actual=%s)\n' \
		"$([ "$ok" = true ] && echo PASS || echo FAIL)" "$name" "$expect" "$actual"
	RESULTS+=("{\"name\":\"$name\",\"expect\":\"$expect\",\"actual\":\"$actual\",\"status\":\"$([ "$ok" = true ] && echo PASS || echo FAIL)\"}")
}

echo ""
echo "--- 0. setup (shared table created on node0, visible everywhere) ---"
q "$N0" "DROP TABLE IF EXISTS demo4 CASCADE"
q "$N0" "CREATE TABLE demo4 (id int primary key, v text)"

echo "--- 1. cross-node INSERT/UPDATE visibility ---"
q "$N0" "INSERT INTO demo4 VALUES (1, 'from_n0')"
assert "n1 sees n0 insert" "from_n0" "$(q "$N1" "SELECT v FROM demo4 WHERE id=1")"
q "$N2" "UPDATE demo4 SET v='from_n2' WHERE id=1"
assert "n3 sees n2 update" "from_n2" "$(q "$N3" "SELECT v FROM demo4 WHERE id=1")"

echo "--- 2. CR historical read (held snapshot on n1 survives n0 commit) ---"
# Open a REPEATABLE READ snapshot on n1, mutate+commit on n0, re-read on n1.
CR_OUT="$(
	"$PSQL" -X -A -t -q -d "$N1" <<SQL
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT 1;
\! $PSQL -X -A -t -q -d "$N0" -c "UPDATE demo4 SET v='cr_changed' WHERE id=1"
SELECT v FROM demo4 WHERE id=1;
COMMIT;
SQL
)"
CR_SEEN="$(echo "$CR_OUT" | tail -1)"
assert "n1 held snapshot reads pre-commit (CR)" "from_n2" "$CR_SEEN"

echo "--- 3. cross-node 2PC (PREPARE on n0, COMMIT PREPARED, visible on n3) ---"
q "$N0" "BEGIN; INSERT INTO demo4 VALUES (2, 'twopc'); PREPARE TRANSACTION 'demo4_2pc'"
assert "n3 cannot see prepared-but-uncommitted" "0" "$(q "$N3" "SELECT count(*) FROM demo4 WHERE id=2")"
q "$N0" "COMMIT PREPARED 'demo4_2pc'"
assert "n3 sees committed 2PC row" "twopc" "$(q "$N3" "SELECT v FROM demo4 WHERE id=2")"

echo "--- 4. retention / undo cleaner recycle (counters advance on n0) ---"
for i in $(seq 1 10); do q "$N0" "UPDATE demo4 SET v='churn_$i' WHERE id=1" >/dev/null; done
CLEANER="$(q "$N0" "SELECT COALESCE((SELECT count(*) FROM pg_cluster_state WHERE category='undo_cleaner'),0)")"
assert "undo_cleaner surface live (>=1 key)" "true" "$([ "${CLEANER:-0}" -ge 1 ] && echo true || echo false)"

echo "--- 5. crash-restart recovery note (manual) ---"
# A real crash-restart needs node-level control (pg_ctl) on the demo host;
# this leg is a placeholder that records the pre/post invariant the operator
# should verify out-of-band.  Recorded as a manual step, not auto-asserted.
RESULTS+=("{\"name\":\"crash_restart_recovery\",\"status\":\"MANUAL\",\"note\":\"operator restarts one node via pg_ctl and re-checks demo4 row count\"}")
echo "  [MANUAL] crash-restart recovery — operator-driven (see note in JSON)"

# Emit JSON
{
	printf '{\n'
	printf '  "spec": "3.17",\n'
	printf '  "demo": "4node-shared-storage",\n'
	printf '  "timestamp": "%s",\n' "$TIMESTAMP"
	printf '  "status": "%s",\n' "$([ "$FAILS" -eq 0 ] && echo PASS || echo FAIL)"
	printf '  "production_safe": false,\n'
	printf '  "results": [\n'
	n=${#RESULTS[@]}
	for (( i=0; i<n; i++ )); do
		printf '    %s' "${RESULTS[$i]}"
		[ "$i" -lt $(( n - 1 )) ] && printf ','
		printf '\n'
	done
	printf '  ]\n'
	printf '}\n'
} > "$OUT_JSON"

echo ""
echo "=== demo JSON written to: $OUT_JSON (failures=$FAILS) ==="
[ "$FAILS" -eq 0 ] || { echo "DEMO FAILED: $FAILS correctness assertion(s) failed" >&2; exit 1; }
echo "all correctness assertions passed"
