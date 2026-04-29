# Bootstrap a node

Once the [install](install.md) is complete, the `pgrac-init` and
`pgrac-start` shell tools provide a one-command path to creating a
PostgreSQL data directory with cluster-aware configuration and
starting the server.

## Quick start (single node)

```bash
# 1. Create PGDATA + write cluster.node_id + write pgrac.conf
pgrac-init -D /tmp/linkdb-demo --node-id=0 --cluster-name=demo

# 2. Adjust port + socket dir if needed
echo "port = 65433"                                  >> /tmp/linkdb-demo/postgresql.conf
echo "unix_socket_directories = '/tmp'"              >> /tmp/linkdb-demo/postgresql.conf
echo "listen_addresses = ''"                         >> /tmp/linkdb-demo/postgresql.conf

# 3. Start the server
pgrac-start -D /tmp/linkdb-demo -l /tmp/linkdb-demo.log -w

# 4. Connect and inspect cluster state
psql -h /tmp -p 65433 -d postgres -c 'SELECT * FROM pg_cluster_nodes;'
#  node_id |        hostname         | interconnect_addr | public_addr |  role   | region | is_self
# ---------+-------------------------+-------------------+-------------+---------+--------+---------
#        0 | <your-hostname>         | 127.0.0.1:6432    |             | primary |        | t

# 5. Stop when done
pg_ctl -D /tmp/linkdb-demo -m fast stop
```

## What `pgrac-init` does

Under the hood `pgrac-init` is a shell wrapper that performs three
steps in sequence:

1. Runs `initdb -D <PGDATA>` to create a fresh PostgreSQL data
   directory (skipped if the directory is already initialised).
2. Appends `cluster.node_id = <N>` to `postgresql.conf` (idempotent
   if the same value is already there; refuses to overwrite a
   different value unless `--force` is passed).
3. Writes `<PGDATA>/pgrac.conf` with a `[cluster]` section and a
   `[node.<N>]` section.  Existing `pgrac.conf` is preserved unless
   `--force` is passed.

See `pgrac-init --help` for the full option list.

## What `pgrac-start` does

`pgrac-start` is a thin wrapper around `pg_ctl start` with three
preflight checks:

1. PGDATA exists and contains `PG_VERSION`.
2. `postgresql.conf` declares `cluster.node_id`.
3. If `pgrac.conf` is present, the first `[node.<N>]` section
   matches `cluster.node_id` (mismatches produce a warning, not a
   hard fail; the postmaster itself FATALs on a true mismatch with
   a precise hint).

After preflight `pgrac-start` exec's `pg_ctl -D <PGDATA> start`
with all `-l / -w / -W / -t / -o` flags passed through unchanged.

## Stop / status / reload

`pgrac-init` and `pgrac-start` only cover the bootstrap and start
paths.  Use the standard `pg_ctl` for everything else:

```bash
pg_ctl -D /tmp/linkdb-demo status        # check status
pg_ctl -D /tmp/linkdb-demo -m fast stop  # graceful stop
pg_ctl -D /tmp/linkdb-demo reload        # reread postgresql.conf (PGC_SIGHUP GUCs only)
```

## Single-node fallback (no `pgrac.conf`)

If `pgrac.conf` is missing entirely (e.g. you ran plain `initdb`
without `pgrac-init`), the server falls back to a single-node
topology containing one row -- the local node, with `node_id`
taken from the `cluster.node_id` GUC.  A `LOG`-level message is
emitted at startup:

```
LOG: cluster_conf: "<path>/pgrac.conf" not found, falling back to
     single-node mode (node_id=<N>)
```

`pg_cluster_nodes` then returns one row with the local node
information.  This makes single-node development convenient: you
do not need to write a `pgrac.conf` to run linkdb as a stand-alone
PostgreSQL server with the cluster GUCs available.

## Current limitations

- **Single node only**: cross-node interconnect is currently not
  supported.  Setting `cluster.interconnect_tier` to anything other
  than `stub` causes the postmaster to refuse to start with
  `ERRCODE_FEATURE_NOT_SUPPORTED`.  See
  [Configuration](configuration.md) for the GUC reference.
- **Bash only**: `pgrac-init` / `pgrac-start` are bash scripts;
  they require a POSIX shell.  Windows is not currently supported.
- **No multi-node bootstrap**: there is no built-in tool for
  initialising N nodes simultaneously across hosts.  Each node
  must be bootstrapped individually with the matching `--node-id`.

## Reporting issues

File issues at <https://github.com/sqlrush/linkdb/issues>.
