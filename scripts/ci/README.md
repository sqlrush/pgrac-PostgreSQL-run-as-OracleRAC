# scripts/ci/ — CI helper scripts

> Author: SqlRush \<sqlrush@gmail.com\>

CI/CD helper scripts invoked by `.github/workflows/ci.yml`.  Designed
to be runnable locally for layer-2 gate (commit-time) checks.

## Scripts

### `check-comment-headers.sh`

Enforces CLAUDE.md rule 11 (comment conventions) on changed files:

1. **Hard errors** (CI fails):
   - Newly-added cluster source files (`src/src/(backend|include)/cluster/**.{c,h}`)
     must contain `Author: SqlRush <sqlrush@gmail.com>`
   - Newly-added cluster test files (`src/src/test/cluster_(unit|tap|regress)/**.{c,h,pl,pm}`)
     must contain the same Author line

2. **Warnings** (CI not failed in stage 0.6, will tighten in 0.7):
   - Modified PG-original files (under `src/src/` outside cluster
     directories) should contain a `PGRAC` marker

#### Local invocation

```bash
# from repo root
./scripts/ci/check-comment-headers.sh
```

The script picks the diff base automatically:
- PR event: `origin/$GITHUB_BASE_REF`
- Push event: `merge-base origin/main HEAD`
- Local: same as push

### `check-format.sh` (stage 0.7)

Enforces `.clang-format` compliance on pgrac cluster sources.  Iterates
the cluster paths (`src/src/backend/cluster/`, `src/src/include/cluster/`,
`src/src/test/cluster_unit/`) and diffs each `.c` / `.h` file against
the output of `clang-format`.  Fails CI if any file has a violation.

PG-original files are NOT checked (they remain pgindent-styled per
upstream convention).

To fix locally: `clang-format -i <file>`.

### `check-tidy.sh` (stage 0.7, warn-only)

Runs `clang-tidy` on cluster `.c` files using the rules in `.clang-tidy`.
Stage 0.7 is **warn-only** -- the script always exits 0 even on
warnings.  Stage 0.27 will tighten this to fail on new warnings.

## Future scripts (placeholder)

| Script | Stage | Purpose |
|---|---|---|
| `check-perf-baseline.sh` | 0.23 | Run pgbench and compare against perf-baseline.md |
| `check-secrets.sh` | 0.27 | git-secrets / detect-secrets wrapper |
| `check-static-analysis.sh` | 0.27 | cppcheck / scan-build wrapper |
| `run-standard-pipeline.sh` | 0.30 | One-shot local equivalent of CI standard pipeline |

## Versioning

Scripts here are part of the pgrac codebase under PostgreSQL License.
Each script's header documents what changed and why.
