# System views

linkdb adds three cluster-aware system views to the standard
PostgreSQL catalog.  All three are present in `--enable-cluster`
builds; in `--disable-cluster` builds they return zero rows.

| View | Purpose |
|---|---|
| `pg_cluster_nodes` | Cluster topology (the parsed `pgrac.conf`) |
| `pg_stat_cluster_wait_events` | Cluster-specific wait events on the local node |
| `pg_stat_gcluster_wait_events` | Cluster wait events globally (cross-node placeholder) |

## pg_cluster_nodes

Returns one row per node declared in `pgrac.conf`.  In single-node
fallback mode (no `pgrac.conf`) returns one row for the local node.

### Columns

| Column | Type | Nullable | Description |
|---|---|---|---|
| `node_id` | integer | NO | Numeric node id `[0, 127]` |
| `hostname` | text | YES | Short hostname (NULL when not declared in pgrac.conf) |
| `interconnect_addr` | text | NO | `"host:port"` of the node's interconnect endpoint.  Empty string in single-node fallback. |
| `public_addr` | text | YES | Client-facing `"host:port"` (NULL when not declared) |
| `role` | text | NO | `primary` / `standby` / `arbiter` |
| `region` | text | YES | Free-form region tag (NULL when not declared) |
| `is_self` | boolean | NO | True if `node_id` matches the local `cluster.node_id` GUC |

### Example queries

```sql
-- All nodes:
SELECT * FROM pg_cluster_nodes ORDER BY node_id;

-- Just this node's row:
SELECT * FROM pg_cluster_nodes WHERE is_self;

-- Counts by role:
SELECT role, count(*) FROM pg_cluster_nodes GROUP BY role;
```

### Sample output (3-node cluster)

```
 node_id |  hostname    | interconnect_addr |    public_addr     |  role   |  region    | is_self
---------+--------------+-------------------+--------------------+---------+------------+---------
       0 | db-1         | 10.0.0.1:6432     | 192.168.1.1:5432   | primary | us-east-1a | t
       1 | db-2         | 10.0.0.2:6432     |                    | standby | us-east-1b | f
       2 | db-3         | 10.0.0.3:6432     |                    | standby | us-east-1c | f
```

## pg_stat_cluster_wait_events

Lists the cluster-specific wait event registry on the local node.
Always returns 46 rows in `--enable-cluster` builds (one per
registered cluster wait event).

### Columns

| Column | Type | Nullable | Description |
|---|---|---|---|
| `type` | text | NO | Wait event class.  Always begins with `Cluster: ` (e.g. `Cluster: GES`, `Cluster: PCM`). |
| `name` | text | NO | Wait event name (e.g. `GesEnqueueAcquire`, `PcmBlockReadNS`). |

### Example queries

```sql
-- Total registered events (must be 46):
SELECT count(*) FROM pg_stat_cluster_wait_events;

-- Distinct classes (must be 10):
SELECT DISTINCT type FROM pg_stat_cluster_wait_events ORDER BY type;

-- Per-class counts:
SELECT type, count(*) FROM pg_stat_cluster_wait_events GROUP BY type ORDER BY type;
```

See [Wait events](wait-events.md) for the full event roster.

## pg_stat_gcluster_wait_events

Cross-node placeholder for cluster-wide wait events.  In the
current release returns 46 rows for the local node only;
`node_id` is always the value of the local `cluster.node_id` GUC.

The column shape `(node_id, type, name)` is the public contract
and will not change when full multi-node fan-out is added in a
future release.

### Columns

| Column | Type | Nullable | Description |
|---|---|---|---|
| `node_id` | integer | NO | Node that observed this wait event.  Currently always equal to local `cluster.node_id`. |
| `type` | text | NO | Same semantics as `pg_stat_cluster_wait_events.type`. |
| `name` | text | NO | Same semantics as `pg_stat_cluster_wait_events.name`. |

### Example queries

```sql
-- Distinct nodes seen in the global view (currently always 1):
SELECT count(DISTINCT node_id) FROM pg_stat_gcluster_wait_events;

-- The (type, name) projection equals pg_stat_cluster_wait_events
-- exactly while the global view contains only local data:
SELECT count(*) FROM (
    SELECT type, name FROM pg_stat_gcluster_wait_events
    EXCEPT
    SELECT type, name FROM pg_stat_cluster_wait_events
) d;
-- 0
```

## --disable-cluster builds

In binaries built with `--disable-cluster`:

- All three views still exist in the catalog.
- All three return zero rows.
- The underlying `cluster_get_*` SRFs are present as no-op symbols.

This means SQL written against these views works on both build
modes; the cluster-specific data is simply absent on a vanilla
build.

## Reporting issues

File issues at <https://github.com/sqlrush/linkdb/issues>.
