# GES BAST + Cross-Node Deadlock — User Manual

> Status: spec-2.17 checkpoint documentation.  The current code exposes
> BAST/CANCEL/deadlock GUCs, counters, opcodes, generation guards, and
> safe ProcSignal dispatch scaffolding.  Caller-side LockAcquireExtended,
> full BAST round trip, cross-node deadlock detection, and CANCEL wait abort
> are not production-active until the remaining spec-2.17 activation work
> lands.

## Overview

spec-2.17 checkpoint work prepares Phase 2.B GRD/LMS/LMD activation by
adding surfaces for three coordination mechanisms:

- **BAST (Blocking Asynchronous Trap)** — master notifies original
  holder advisory when new requester wants incompatible mode;
  **holder is NOT killed**, decisions stay with the holder backend
  (per PG ACID semantics);  natural `LockRelease` path supplements
  with `GES_RELEASE` when refcount reaches 0.
- **Cross-node deadlock detector** — coordinator periodically probes
  all nodes for wait-for graph edges, runs cycle detection, selects
  deterministic victim by age-based 4-tuple ordering.
- **CANCEL ProcSignal** — 3 sources(lock_timeout self / deadlock
  victim / `pg_cancel_backend`)abort wait;  pending requests use
  `GES_CANCEL_PENDING`,  granted holders use `GES_RELEASE`.

## Configuration

| GUC | Default | Range | Context | Description |
|---|---|---|---|---|
| `cluster.ges_bast_retry_interval_ms` | `10000` | `[1000, 60000]` | `SIGHUP` | BAST retry interval (ms). Master 重发周期(不是 kill 阈值) |
| `cluster.ges_bast_max_retries` | `3` | `[1, 10]` | `SIGHUP` | Max BAST retry attempts before REJECT to new requester |
| `cluster.ges_deadlock_check_interval_ms` | `1000` | `[100, 10000]` | `SIGHUP` | Deadlock probe periodic interval |
| `cluster.ges_deadlock_chunk_timeout_ms` | `2000` | `[500, 30000]` | `SIGHUP` | Chunked reassembly timeout (drop entire probe on miss) |
| `cluster.ges_deadlock_max_edges` | `1024` | `[64, 65536]` | `SIGHUP` | Max wait-for edges per probe (hard cap protects LMON) |
| `cluster.ges_deadlock_max_vertices` | `256` | `[16, 16384]` | `SIGHUP` | Max vertices per probe |
| `cluster.ges_deadlock_max_in_flight_probes` | `4` | `[1, 32]` | `SIGHUP` | Max concurrent in-flight probes per coordinator |
| `cluster.ges_deadlock_tick_budget_us` | `5000` | `[500, 50000]` | `SIGHUP` | Single LMON tick deadlock work budget (us) |

## Wait Events

| Name | Class | When |
|---|---|---|
| `GesBastWait` | Cluster:GES | Master waiting BAST_ACK from original holder |
| `GesDeadlockProbeWait` | Cluster:GES | Coordinator waiting probe replies |
| `GesCancelDrain` | Cluster:GES | Backend draining CANCEL outbound |
| `GesDeadlockReassemblyWait` | Cluster:GES | Coordinator waiting chunk reassembly |

## SQLSTATE Error Codes

| Code | Name | Meaning |
|---|---|---|
| `53R72` | `cluster_ges_deadlock` | Backend chosen as victim by cross-node deadlock detector |
| `53R73` | `cluster_ges_cancel_pending` | Pending reservation cancelled before grant |

## Observability

NEW 9 `pg_cluster_state` rows in the `grd` category:

**BAST counters(6)**:
- `grd_bast_sent_count` — Master 发出 BAST 数
- `grd_bast_received_count` — Holder 收到 BAST + validation pass
- `grd_bast_ack_count` — Natural release path 补发 GES_RELEASE
- `grd_bast_retry_count` — Master 重发次数
- `grd_bast_reject_count` — 超 retry 给 new requester reply REJECT
- `grd_bast_stale_drop_count` — Holder 收 stale BAST 6-tuple validation 失败 drop

**Deadlock counters(3)**:
- `grd_deadlock_probe_drop_count` — Partial chunk timeout → drop entire probe
- `grd_deadlock_probe_collision_drop_count` — probe_id collision drop
- `grd_deadlock_chunk_oo_buffer_overflow_count` — Out-of-order chunk buffer overflow

## Checkpoint Surfaces

- **Safe ProcSignal scaffolding** for BAST/CANCEL: signal context only sets pending flags; real work is consumed from `ProcessInterrupts`.
- **BAST advisory flag**: current handler marks `cluster_grd_bast_pending`; it does not actively release locks.
- **Reserved deadlock protocol knobs and counters**: GUCs and counters exist; Tarjan/cycle detection and victim selection are not active in this checkpoint.
- **Generation guard substrate**: `PGPROC.cluster_grd_generation` gives future BAST/CANCEL payload validation a stale-signal discriminator.

## MVP Scope

**MVP only ADVISORY locks**(`pg_advisory_lock(int)` family).  RELATION /
TRANSACTION / OBJECT class will be added when:

- RELATION/OBJECT — `relpersistence` cache(`temp` / `unlogged` / `permanent` discrimination)— spec-2.18+.
- TRANSACTION — cluster-aware xid encoding(`{origin_node_id, local_xid, cluster_epoch}`)— spec-2.18+.

## Forward Compatibility

- `cluster.ges_request_timeout_ms` range will extend to include `-1`
  (perpetual wait) once retransmit / lease / master-generation ship in
  spec-2.18+.
- 4-tuple vertex record forward-compat for Stage 6 DRM.
- BAST `target_generation` + `request_seq` 防 stale window forward-
  compat with backend procno reuse scenarios.
