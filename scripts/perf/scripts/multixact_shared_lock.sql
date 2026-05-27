-- spec-3.6 D12 — perf workload class 6:  2node-multixact-shared-lock.
--
-- Concurrent FOR SHARE on same row to force MultiXact composition.
-- pgbench fires multiple backends per node;  2+ FOR SHARE on the
-- same row triggers PG's MultiXactIdCreate/Expand path,  which
-- exercises spec-3.6 D5 hook (V4 overlay emit) + D7b page-marker
-- stamp + reader D6 cluster_multixact_resolve_visibility.
--
-- pgbench vars (provided by run_2node_baseline.pl):
--   :scale   pgbench scale factor
--   :node_id 0 or 1 (per-node partitioning)
--
-- Workload mixes shared locks (lock-only members, status 0-3) with
-- occasional update (Update/NoKeyUpdate members, status 4-5) so the
-- resolve_visibility truth table is exercised.
\set hot_aid random(1, 100)
\set delta random(-50, 50)

BEGIN;
SELECT abalance FROM pgbench_accounts WHERE aid = :hot_aid FOR SHARE;
UPDATE pgbench_branches SET bbalance = bbalance + :delta
  WHERE bid = (:hot_aid % 10) + 1;
COMMIT;
