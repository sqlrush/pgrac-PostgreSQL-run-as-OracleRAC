# Per-thread WAL layout

In a cluster, every node writes its own WAL stream ("WAL thread").
Each stream lives in its own directory under one shared root so that
any node can read any stream:

```
<wal_threads_dir>/
├── thread_1/            # node 0's stream (thread id = node_id + 1)
│   ├── 000000010000000000000001
│   ├── ...
│   └── pgrac_thread.claim
└── thread_2/            # node 1's stream
    └── ...
```

Inside each thread directory the files are standard PostgreSQL WAL
segments; `pg_waldump` and all other WAL tooling work unchanged.  Every
WAL page is stamped with the writing node's thread id, shown by
`pg_waldump` as `thread: N` on each record (`thread: 0` means the page
was written outside cluster mode, e.g. by `initdb` or `pg_resetwal`;
mixed pages are normal).

## Setting it up

Pass `--wal-threads-dir` to `pgrac-init` when creating the node:

```bash
pgrac-init -D /data/node0 --node-id=0 --wal-threads-dir=/shared/walroot
pgrac-init -D /data/node1 --node-id=1 --wal-threads-dir=/shared/walroot
```

This relocates `pg_wal` via the standard `initdb -X` mechanism (a
symlink from `$PGDATA/pg_wal` to `<dir>/thread_<id>`) and sets
`cluster.wal_threads_dir` in `postgresql.conf`.  The root must be an
absolute path on storage that all nodes can reach.

An already-initialised data directory cannot be relocated by
`pgrac-init`; stop the server, move the contents of `pg_wal` into the
thread directory, replace `pg_wal` with a symlink to it, and set the
GUC.

## Startup validation

When `cluster.wal_threads_dir` is set, the server refuses to start
(fail-closed) unless the routing is coherent:

| Error | Meaning | Fix |
|---|---|---|
| SQLSTATE `53RA0` | `pg_wal` does not resolve to this node's thread directory, the directory is missing, or the configuration is contradictory (`cluster.enabled=off`, or `cluster.node_id` unset) | Re-link `pg_wal`, create the node with `pgrac-init --wal-threads-dir`, or fix the GUCs |
| SQLSTATE `53RA1` | The thread directory is claimed by a different node, or the claim file is corrupt / cannot be created | Each thread directory belongs to exactly one node.  After confirming ownership, remove `pgrac_thread.claim` and restart; it is never rebuilt automatically |

The claim file is written once on the first successful start and only
ever read back afterwards.

## Observability

`pg_cluster_state` exposes a `wal_thread` category:

| key | meaning |
|---|---|
| `thread_id` | this node's thread id (0 when not clustered) |
| `dir_configured` | `cluster.wal_threads_dir` is set |
| `dir_validated` | startup validation passed |
| `claim_created` | this boot created the claim file (first boot only) |
| `page_stamp_count` | WAL pages stamped with the real thread id since startup |

Two wait events cover the claim file I/O:
`ClusterWalThreadClaimRead` and `ClusterWalThreadClaimWrite`.

## Compatibility notes

- Setting `cluster.wal_threads_dir` is optional.  Without it the flat
  `pg_wal` layout is unchanged; WAL pages are still stamped with the
  node's thread id whenever `cluster.enabled` is on and
  `cluster.node_id` is set.
- **Once a clustered node has written thread-stamped WAL, older pgrac
  binaries (pre per-thread WAL) refuse to read that WAL and fail
  closed at recovery.  Downgrading the binary after enabling cluster
  mode is not supported.**  Vanilla PostgreSQL binaries cannot read
  pgrac data directories in any case.
- Physical streaming replication is unaffected: a standby replays the
  primary's stamped stream regardless of its own `cluster.node_id`.
- `pg_basebackup` from a relocated node produces a backup with a plain
  (empty) `pg_wal` directory, as with any symlinked `pg_wal`.
- `pg_resetwal` writes pages without a thread stamp (`thread: 0`); the
  server resumes stamping on the next start.  Mixed segments are
  valid.
