# Shared temp directories across test *processes*

Failure mode: green-but-wrong and intermittent. A test writes a file and the write fails or the read-back is empty because a different process deleted or truncated it (issue #789). Applies to any test that writes files or reads repo files. For flakes that do not touch disk see timed-wait-test-assertions.md.

## Rules

1. **Never call `std::filesystem::temp_directory_path()` in a test; use `OloEngine/tests/TestTempDir.h`.** `gtest_discover_tests` runs every gtest case as its own `OloEngine-Tests.exe` process, and CI runs them concurrently (`ctest --parallel 4` on Windows, `--parallel 2` in the four sanitizer jobs), so a fixed temp path is shared with sibling cases, concurrent runs of the same binary (two worktrees) and stale trees. [Details](shared-temp-dir-test-isolation-details.md#1-why-a-tests-temp-path-is-shared-state)
2. **Use `OloEngine::Tests::TempDir()`, `TempDir("staging")` and `TempFile("cfg.yaml")`.** `TempDir()` is the per-process, per-gtest-case directory (created); `TempFile()` names a file inside it, parent created, file not. The root is `<system temp>/OloEngineTests-<pid>_<random64>`, claimed exclusively and removed at exit; `--olo-keep-temp` keeps it. [Details](shared-temp-dir-test-isolation-details.md#2-the-contract-never-call-temp_directory_path-in-a-test)
3. **Do not hand-roll a PID suffix and do not group roots under a shared `OloEngineTests/` parent.** The helper root already has PID plus random tag; a fixed-name directory in sticky, world-writable `/tmp` belongs to whoever created it first (other CI accounts). [Details](shared-temp-dir-test-isolation-details.md#2-the-contract-never-call-temp_directory_path-in-a-test)
4. **Opt out of the `test-temp-dir-isolated` pre-commit hook only with `// OLO_TEMP_DIR_OK: <reason>`.** The hook (`OloEngine/tests/scripts/check_temp_dir_isolation.py`) fails any direct `temp_directory_path()` under `OloEngine/tests`; the comment goes on the same or preceding line and says why. [Details](shared-temp-dir-test-isolation-details.md#2-the-contract-never-call-temp_directory_path-in-a-test)
5. **Use `TempFile("x")`, not `TempDir()`, when the call site needs the path to be absent.** `TempDir()` creates the directory; `DataRoundTripTest.IblCache*` probes non-existence for `ScopedIBLCacheGuard` (SonarQube cpp:S5443). [Details](shared-temp-dir-test-isolation-details.md#the-migrations-own-regression-and-what-caught-it)
6. **Do not delete a fixture's `SetUp` cleanup assuming `TempDir()` is fresh across `--gtest_repeat`.** `TempDir()` empties a leaf on its first call within each test and leaves it alone afterwards, because helpers like `MakeUniqueScratchDir()` call it repeatedly. [Details](shared-temp-dir-test-isolation-details.md#the-migrations-own-regression-and-what-caught-it)
7. **When replacing N hand-rolled implementations with one helper, keep every property they had, write the helper's contracts as tests, and run the whole suite.** Here a clean slate per test and a not-yet-existing path were each invisible in the deleted code and each caught by one test; a targeted 18,000-case hammer stayed green. [Details](shared-temp-dir-test-isolation-details.md#the-migrations-own-regression-and-what-caught-it)
8. **Serialize non-test-controlled shared resources with a ctest `RESOURCE_LOCK`.** The engine's repo-relative mesh cache under `OloEditor/assets/cache/mesh/` cannot be process-keyed (testing-architecture.md §6.2). [Details](shared-temp-dir-test-isolation-details.md#6-adding-a-test-that-writes-files)
9. **Back a safety property asserted in a comment with a mechanism, or state its real scope.** "The suite is parallel-safe by construction" (asan.yml, tests CMakeLists, testing-architecture.md §6.1) was never measured; name the hook, test or generator that keeps a claim true. [Details](shared-temp-dir-test-isolation-details.md#4-the-ci-comment-that-documented-a-property-nobody-had-checked)
10. **Check a cited flake's date against the fix's merge date before treating it as a live repro.** The #789 repro (`FrameExportTest`) predated its fix `fbbc244b`. [Details](shared-temp-dir-test-isolation-details.md#3-the-audit-that-produced-this-doc--and-why-the-headline-number-was-wrong)
11. **Do not count only PID/TID/GUID keying as safe; case-name keying is an approved pattern.** Audit by class: A keyed, B fixed path used by one case, C fixed path shared by 2+ cases (the real race: five GPU-gated `RendererAttachedTest::BuildScene()` sites writing `AssetRegistry.oar`, local-only because `SetUp` skips headless). [Details](shared-temp-dir-test-isolation-details.md#3-the-audit-that-produced-this-doc--and-why-the-headline-number-was-wrong)
12. **Reproduce a cross-process race with concurrent processes, not a green single run.** 8 concurrent `OloEngine-Tests.exe` over `EngineSubsystemSmoke.*:LocalizationFixture.*:StreamingRegionSerializer.*` at `--gtest_repeat=5` gave 350 failures before the fix and 0 after; the failure text ("Failed to load project file ... Project::Load failed on the just-saved file") names nothing about a race. [Details](shared-temp-dir-test-isolation-details.md#5-what-the-fix-was-measured-against)
13. **Resolve repo files from `OLO_TEST_EDITOR_ROOT`, never relative to the working directory.** Asset-manager-backed Functional fixtures `chdir` to a temp project, so a CWD-relative open passes alone and fails after them in the suite. Use `std::filesystem::path{ OLO_TEST_EDITOR_ROOT }.parent_path() / "OloEngine" / ...` (see the local `RepoRoot()` in `ComponentSerializerCoverageTest`, `ComponentTupleCoverageTest`, `OloHeaderToolOutputsTest`). [Details](shared-temp-dir-test-isolation-details.md#7-the-sibling-hazard-reading-a-repo-file-relative-to-the-working-directory)

## Commands and checks

```
rm -rf /tmp/OloEngineTests-*
```

Sweeps leftover per-process test roots.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[shared-temp-dir-test-isolation-details.md](shared-temp-dir-test-isolation-details.md).
