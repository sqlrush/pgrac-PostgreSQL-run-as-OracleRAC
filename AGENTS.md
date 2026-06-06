# Agent Review Notes

These rules apply to future agent code review and CI-prep work in this repository.

## Assertion And CI Safety

- Treat new or modified `Assert()` calls as code review targets, not passive debug aids.
- For every new `Assert()`, verify both debug/cassert and release-build behavior. Required runtime safety must not depend on `Assert()` being enabled.
- Do not put the only bounds check, slot ownership check, or corruption guard inside `Assert()`. Pair it with an explicit production branch when incorrect state can reach runtime input, WAL replay, recovery, crash restart, or test harnesses.
- Check whether the object file containing the `Assert()` is linked into standalone cluster unit tests under `src/test/cluster_unit`. Pure unit binaries must not accidentally require backend-only symbols such as `ExceptionalCondition`.
- Avoid side effects inside `Assert()` expressions. The expression may disappear in non-assert builds.
- For arrays, slot tables, wrap counters, WAL opcodes, and exported record buffers, review off-by-one behavior explicitly. If a helper can write a sentinel or overflow marker, size the receiving storage for that contract and test the boundary.
- Before declaring CI-ready, run or account for the gates most likely to expose assertion issues: cluster unit tests, changed TAP tests, Linux enable-cluster build, and any relevant debug/cassert path.
