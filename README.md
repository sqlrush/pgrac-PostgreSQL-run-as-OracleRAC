# linkdb

linkdb is a PostgreSQL 16.13 fork that adds cluster-aware features:
shared-disk multi-node deployment, a cluster topology layer, an
inter-node IPC abstraction, and the SQL surface (system views,
GUCs, wait events, error codes) needed to operate them.  It targets
the same problem space as Oracle Real Application Clusters.

The disable-cluster build is binary-equivalent to upstream
PostgreSQL 16.13 and passes the standard 219-test regression suite
unchanged.

## Status

**Stage 0 in progress.**  Cluster subsystem scaffolding is in place:
GUCs / shmem / IPC abstraction / topology / system views / wait
events / pgrac-init bootstrap tools.  Cross-node functionality
(GES / PCM / Cache Fusion / Reconfiguration / Recovery) is
scaffolded but not yet active; operations that would require it
return `ERRCODE_FEATURE_NOT_SUPPORTED`.

## Documentation

User-facing manual:

| Topic | File |
|---|---|
| Installation | [docs/user-guide/install.md](docs/user-guide/install.md) |
| Bootstrap a node | [docs/user-guide/bootstrap.md](docs/user-guide/bootstrap.md) |
| Configuration (`cluster.*` GUCs + `pgrac.conf`) | [docs/user-guide/configuration.md](docs/user-guide/configuration.md) |
| System views reference | [docs/reference/system-views.md](docs/reference/system-views.md) |
| Wait events reference | [docs/reference/wait-events.md](docs/reference/wait-events.md) |
| Architecture overview | [docs/architecture/overview.md](docs/architecture/overview.md) |

PostgreSQL upstream documentation lives under `doc/` and is
shipped unchanged from the upstream tree.

## Quick start

```bash
git clone https://github.com/sqlrush/linkdb.git
cd linkdb

./configure --prefix=$HOME/linkdb-install \
            --enable-cluster --enable-tap-tests \
            --with-openssl --with-icu --with-lz4 --with-zstd
make -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)
make install

export PATH=$HOME/linkdb-install/bin:$PATH

pgrac-init -D /tmp/linkdb-demo --node-id=0 --cluster-name=demo
echo "port = 65433"                                  >> /tmp/linkdb-demo/postgresql.conf
echo "unix_socket_directories = '/tmp'"              >> /tmp/linkdb-demo/postgresql.conf
echo "listen_addresses = ''"                         >> /tmp/linkdb-demo/postgresql.conf

pgrac-start -D /tmp/linkdb-demo -l /tmp/linkdb-demo.log -w
psql -h /tmp -p 65433 -d postgres -c 'SELECT * FROM pg_cluster_nodes;'
```

See [docs/user-guide/bootstrap.md](docs/user-guide/bootstrap.md)
for details.

## Building from source

The build follows the standard PostgreSQL `configure` + `make` +
`make install` flow.  Two extra flags are linkdb-specific:

- `--enable-cluster` activates the cluster subsystem.
- `--enable-tap-tests` enables the TAP test suites (Perl).

See [docs/user-guide/install.md](docs/user-guide/install.md) for
the complete dependency list and step-by-step instructions on
macOS and Linux.

## License

PostgreSQL License (BSD-style).  See `LICENSE` and `COPYRIGHT`.

## Reporting issues

File issues at <https://github.com/sqlrush/linkdb/issues>.

## Upstream

Forked from PostgreSQL 16.13 (<https://www.postgresql.org>).
