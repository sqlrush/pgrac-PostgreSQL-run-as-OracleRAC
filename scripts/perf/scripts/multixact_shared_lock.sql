-- spec-3.6 D12 — perf workload class 6:  2node-multixact-shared-lock.
--
-- Concurrent FOR SHARE on the same hot row to force MultiXact composition.
-- 2+ backends locking same row triggers PG's MultiXactIdCreate path,
-- which exercises spec-3.6 D5 hook (V4 overlay emit) + D7b marker stamp
-- + reader D6 cluster_multixact_resolve_visibility.
--
-- Workload notes (spec-3.6 v0.3 Sprint A Step 12 真测 hardening):
--   - Single FOR SHARE per tx (no UPDATE alongside) so ITL slot pressure
--     stays under INITRANS=8 cap.
--   - Hot row choice spreads over 100 ids to balance MultiXact
--     composition rate vs ITL slot reuse;  too narrow → overflow,
--     too wide → no MultiXact compose.
--
-- pgbench vars:
--   :scale   pgbench scale factor
--   :node_id 0 or 1 (per-node partitioning)
\set hot_aid random(1, 100)

BEGIN;
SELECT abalance FROM pgbench_accounts WHERE aid = :hot_aid FOR SHARE;
COMMIT;
