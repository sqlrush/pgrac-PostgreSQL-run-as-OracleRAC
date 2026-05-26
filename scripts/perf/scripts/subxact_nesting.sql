-- spec-3.5 D15 — perf workload class 5:  2node-subxact-nesting.
--
-- Savepoint depth=5 nesting workload.  Each transaction opens five
-- savepoints, performs a small UPDATE inside the innermost, then
-- releases the chain in LIFO order before COMMIT.  Exercises the
-- spec-3.5 SUBCOMMITTED emit + parent_key chain path under repeated
-- subxact lifecycle.
--
-- pgbench vars (provided by run_2node_baseline.pl):
--   :scale     pgbench scale factor
--   :node_id   0 or 1 (per-node partitioning;  reuses local_affinity
--              key-range pattern to keep base UPDATE cheap so we
--              measure subxact cost rather than IC traffic)
\set aid_range_start (:node_id * 100000 * :scale / 2)
\set aid_range_end ((:node_id + 1) * 100000 * :scale / 2)
\set aid random(:aid_range_start, :aid_range_end - 1)
\set delta random(-5000, 5000)

BEGIN;
SAVEPOINT s1;
  SAVEPOINT s2;
	SAVEPOINT s3;
	  SAVEPOINT s4;
		SAVEPOINT s5;
		  UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
		RELEASE SAVEPOINT s5;
	  RELEASE SAVEPOINT s4;
	RELEASE SAVEPOINT s3;
  RELEASE SAVEPOINT s2;
RELEASE SAVEPOINT s1;
COMMIT;
