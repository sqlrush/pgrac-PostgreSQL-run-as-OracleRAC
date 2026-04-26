# pgrac cluster regression tests

This directory will host pgrac's own SQL-driven regression tests
(modeled after PG's `src/test/regress`).  They cover cluster-level
behavior that requires a running PG instance — Cache Fusion message
flow, GES enqueue acquisition, SCN piggyback propagation, etc.

**Status (stage 0.4)**: placeholder.  No tests yet.  The directory
skeleton, Makefile, and SUBDIRS wiring are in place so that future
feature points can drop in `*.sql` / `expected/*.out` files without
revisiting build infrastructure.

## When to add tests here

| Subsystem | Likely stage |
|---|---|
| Heartbeat / membership | Stage 2 |
| GRD master selection | Stage 2 |
| PCM block transitions | Stage 2 |
| Cache Fusion 2-way / 3-way | Stage 2 |
| SCN piggyback | Stage 2 |
| Sinval cross-node broadcast | Stage 2 |
| Undo / TT cross-node | Stage 3 |
| Recovery (merged WAL apply) | Stage 4 |
| GES locks (TX / TM / SQ / ...) | Stage 5 |

## Distinction vs unit tests

| | `cluster_regress/` | `cluster_unit/` |
|---|---|---|
| Driver | SQL via `pg_regress` | C `main()` per binary |
| Scope | end-to-end (needs running PG) | function-level (no PG process) |
| Multi-node | yes (later stages) | no |
| TAP output | optional | yes (default) |

## Author

SqlRush \<sqlrush@gmail.com\>
