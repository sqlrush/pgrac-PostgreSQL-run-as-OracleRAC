-- spec-2.7 D9: smgr_cluster invalidation hook smoke (cluster_regress).
--
--   Catalog-level surface check for the pieces spec-2.7 v0.2
--   added in pgrac:specs/spec-2.7-smgr-cluster-2node-concurrent-open.md
--   (frozen 2026-05-09 Q1-Q8 user approve):
--     - cluster.smgr.remote_invalidation_stub_call_count counter
--       readable via pg_stat_cluster_counters (Q5 v0.2 name)
--     - shared_fs.smgr_active_relations key still readable via
--       cluster_dump_state() (regression check after Step 1+2 hook
--       additions)
--     - cluster.smgr_user_relations GUC visible default off (Q4
--       EXPERIMENTAL guardrail still in place)
--
-- Runtime hook exercise (counter > 0 after CRUD, segfault freedom on
-- truncate, etc.) is TAP-tested in
-- t/090_smgr_cluster_2node_concurrent_open.pl (D8).
--
-- ----------
-- Block 1: spec-2.7 D6 atomic counter readable via the
-- pg_stat_cluster_counters view (registry mirror sync).
-- ----------
SELECT count(*)
  FROM pg_stat_cluster_counters
 WHERE name = 'cluster.smgr.remote_invalidation_stub_call_count';

-- ----------
-- Block 2: cluster_smgr_active_relation_count surface still wired
-- through pg_cluster_state (Stage 1.2 contract preserved).
-- ----------
SELECT count(*)
  FROM cluster_dump_state()
 WHERE category = 'shared_fs' AND key = 'smgr_active_relations';

-- ----------
-- Block 3: cluster.smgr_user_relations GUC visible + default off
-- (spec-1.7.1 EXPERIMENTAL flag preserved in spec-2.7 Q4).
-- ----------
SELECT name, setting
  FROM pg_settings
 WHERE name = 'cluster.smgr_user_relations';
