# Shared temp directories across test *processes*

**Failure mode:** green-but-wrong, and *intermittent* — a test writes a file, and the write silently
fails or the read-back comes up empty, because a different **process** deleted or truncated the file
between the two. Nothing in the output says "race". Issue #789.

Sibling doc: [timed-wait-test-assertions.md](timed-wait-test-assertions.md) covers the *other* flake
family in this suite — same genre (a rare red charged to whatever PR was running), different
mechanism (clock granularity, not the filesystem). If a test flakes and it does **not** touch disk,
start there instead.

---

## 1. Why a test's temp path is shared state

`gtest_discover_tests` registers **every** gtest case as its own ctest entry
([`OloEngine/tests/CMakeLists.txt`](../../OloEngine/tests/CMakeLists.txt)). Each case therefore runs
in its own `OloEngine-Tests.exe` **process**, and CI runs those processes concurrently:
`ctest --parallel 4` on Windows, `--parallel 2` in the three sanitizer jobs.

So a path like

```cpp
std::filesystem::temp_directory_path() / "olo_widget_test"
```

is not scratch space. It is a name in a namespace shared with:

1. **every sibling case of the same fixture** — they run at the same time, in different processes,
   and a `SetUp` that does `remove_all` + `create_directories` on that path deletes a concurrent
   case's files mid-write;
2. **a second concurrent run of the same binary** — two worktrees on one box, or a local run
   alongside CI. Case-name keying does *not* save you here: both runs use the same case names;
3. **a stale tree left by a crashed earlier run** — which a PID-keyed path will happily adopt,
   because PIDs are recycled.

The symptom is always downstream of the race: `!ofstream.is_open()`, a `false` from a serializer, an
empty read-back, a YAML parse of a half-written file. It reproduces rarely, so each occurrence looks
like a property of whichever PR was running.

## 2. The contract: never call `temp_directory_path()` in a test

Use [`OloEngine/tests/TestTempDir.h`](../../OloEngine/tests/TestTempDir.h):

```cpp
#include "TestTempDir.h"

OloEngine::Tests::TempDir()             // this process, this gtest case — created
OloEngine::Tests::TempDir("staging")    // ...a second directory for the same case
OloEngine::Tests::TempFile("cfg.yaml")  // a file inside TempDir(); parent created, file not
```

The root is `<system temp>/OloEngineTests-<pid>_<random64>`, **claimed exclusively** with
`create_directory` (a losing claim retries with a fresh suffix, so a recycled PID can never adopt a
stale tree) and `remove_all`'d at normal process exit. Set `OLO_TEST_KEEP_TEMP=1` to keep it for a
post-mortem.

Three properties matter and none is free:

- **Exclusive claim, not just a PID.** PID alone fails against a crashed run's leftovers. Random
  alone fails to identify the owner in a post-mortem. The root carries both.
- **Case keying underneath the process root, not instead of it.** Keying only by gtest case name is
  safe *within* one run and unsafe across two — which is precisely the configuration this box is in
  with several worktrees.
- **A flat `OloEngineTests-` prefix, not a shared `OloEngineTests/` parent.** Grouping the
  per-process roots under one directory reads tidier and is the same bug one level up: a
  fixed-name directory in a sticky, world-writable `/tmp` belongs to whoever created it first, and
  nobody else can create inside it. This repo's self-hosted CI runs as its own account alongside
  other users — `AssetSceneLoadTest` had already been bitten by exactly this and said so in a
  comment. The prefix sweeps just as easily (`rm -rf /tmp/OloEngineTests-*`).

A pre-commit hook, `test-temp-dir-isolated`
([`OloEngine/tests/scripts/check_temp_dir_isolation.py`](../../OloEngine/tests/scripts/check_temp_dir_isolation.py)),
fails the commit on a direct `temp_directory_path()` call anywhere under `OloEngine/tests`. A site
that genuinely must name the system temp root opts out with `// OLO_TEMP_DIR_OK: <reason>` on the
same or preceding line — say *why*, so the next reader can tell a decision from an oversight.

## 3. The audit that produced this doc — and why the headline number was wrong

Issue #789 reported "144 of 154 sites are fixed shared paths". That count came from grepping for
PID / TID / GUID and calling everything else unsafe. Measured properly, the tree looked like this
(150 `temp_directory_path` sites in `OloEngine/tests`, 68 files, at `d396ff03`):

| Class | Sites | Safe within one run? | Safe across two concurrent runs? |
|---|---:|---|---|
| **A** — keyed by PID / TID / nanos+counter / random | 60 | yes | yes |
| **B** — fixed path, used by exactly **one** gtest case | 79 | **yes** — only one process ever names it | no |
| **C** — fixed path in fixture/helper scope, shared by **≥2** cases | 5 | **no — the real race** | no |
| **D** — a mention, not a call (an error string) | 1 | n/a | n/a |

**Class C, the whole intra-run residue, is five sites — all GPU-gated:**

| Site | Shared path | Cases |
|---|---|---:|
| `Rendering/PropertyTests/VirtualGeometryVisualEvidenceTest.cpp:258` | `OloEngineVirtualGeometryEvidence` | 11 |
| `Rendering/PropertyTests/SubmeshMaterialPathParityTest.cpp:654` | `OloEngineSubmeshMaterialParity` | 3 |
| `Rendering/PropertyTests/SubmeshMaterialPathParityTest.cpp:210` | `OloEngineSubmeshMaterialFixture` | 3 |
| `Rendering/PropertyTests/VirtualGeometryRasterParityTest.cpp:281` | `OloEngineVirtualGeometryRasterParity` | 2 |
| `Rendering/PropertyTests/PerfRegressionTests.cpp:1181` | `OloEngineVirtualGeometryPerf` | 2 |

Each is a `RendererAttachedTest::BuildScene()` that bootstraps a throwaway project at a fixed path,
then hands it to an `EditorAssetManager` — which writes `AssetRegistry.oar` **into that directory**.
Eleven processes writing one registry file is a genuine cross-process race. It has never been seen
on CI for one reason: `RendererAttachedTest::SetUp` calls `GTEST_SKIP()` before `BuildScene()` when
there is no GL 4.6 context, so on headless CI these cases never touch the path at all. **The bug is
local-only, and the local runs are the ones nobody watches.**

**Two things the issue got wrong, both worth remembering:**

- **The named repro was already fixed.** Both cited false reds are `FrameExportTest` cases, and the
  per-case-subdir fix for that fixture landed in `fbbc244b` (2026-06-24 11:49). The master failure
  in run `28098055255` ran on `3c30f582`, which does **not** contain that commit. The evidence was
  real; it just predated its own fix. *Check a cited flake's date against the fix's merge date
  before treating it as a live repro target.*
- **Case-name keying was not counted as uniquification**, although
  [testing-architecture.md §6.1](testing-architecture.md) has always blessed it as one of the two
  approved patterns. That is where 79 of the "144 unsafe" sites went.

Class B was migrated anyway. Not because those sites were racing, but because a rule with 79
individually-argued exceptions is not a rule the next author can follow, and because "every temp
path is keyed by PID or gtest case name" then becomes a claim a **grep** can check — which is what
turns the CI comment (§4) from folklore into something enforced.

## 4. The CI comment that documented a property nobody had checked

[`.github/workflows/asan.yml`](../../.github/workflows/asan.yml) justified its parallel run with
"the suite is parallel-safe by construction: tests key their temp paths by PID / gtest case name".
The same claim sat in `OloEngine/tests/CMakeLists.txt` ("most tests are process-isolated already")
and in testing-architecture.md §6.1.

It was *nearly* true and had never been measured — the gap was the five class-C sites, plus the
whole cross-run axis nobody had considered. A claim like that is load-bearing: it is what the next
person reads before deciding to raise `-j`. The rule this leaves behind:

> **A safety property asserted in a comment must be enforced by something, or it is a guess with
> good typography.** If you write one, name the mechanism that keeps it true in the same breath — a
> hook, a test, a generator. If there isn't one, write down the actual scope ("holds for X, not
> Y") instead of the aspiration.

## 5. What the fix was measured against

A green run proves nothing about a rare race, so the fix was pinned by a before/after at identical
parameters, on the same box, with the pre-fix binary kept around to run the "before" leg.

**The cross-run mechanism reproduces on demand.** Eight concurrent `OloEngine-Tests.exe` processes
over `EngineSubsystemSmoke.*:LocalizationFixture.*:StreamingRegionSerializer.*` (41 cases) at
`--gtest_repeat=5`:

| | Failures |
|---|---:|
| single process, same filter | **0** (41/41 in 1.5 s) |
| 8 concurrent processes — **before** | **350** (60/30/50/26/52/38/34/60) |
| 8 concurrent processes — **after** | **0** |
| 12 processes × 20 repeats × 75 cases (18,000 case runs) — after | **0** |

The failure text is worth knowing by sight, because it names nothing that suggests a race:

```
[ RUN      ] EngineSubsystemSmoke.ProjectSaveActiveLoadRoundTrip
Failed to load project file 'C:\...\Temp\OloEngineSubsystemSmoke_roundtrip\RoundTrip.oloproj'
EngineSubsystemSmokeTest.cpp(163): error: Value of: reloaded
  Actual: false
Project::Load failed on the just-saved file.
```

**The class-C intra-run race did not reproduce.** Four different `VirtualGeometryVisualEvidence`
cases run concurrently × 6 rounds (24 GPU runs) came back clean. Those five sites are shared by
construction — the code is unambiguous — but the write window is small and identical content is
written by every case, so a failure never surfaced. Fixed on the structural argument, not on a
reproduction; if you go looking for it, that is the state of the evidence.

### The migration's own regression, and what caught it

Deleting each fixture's `SetUp` `remove_all` on the grounds that `TempDir()` hands back a fresh
directory was **wrong in one case**: under `--gtest_repeat` the same case runs several times in one
process and resolves to the same leaf, so the second iteration inherited its own leftovers. CI never
uses `--gtest_repeat`, and the whole migrated suite stayed green — the only thing that failed was
the helper's *own* test, which asserts `TempFile()` names a path that does not exist yet.

The fix belongs in the helper, not the test: `TempDir()` now empties a leaf on its **first** call
within each test (a gtest listener clears the bookkeeping at `OnTestStart`) and leaves it alone
afterwards, because helpers like `MakeUniqueScratchDir()` call it repeatedly and need earlier
content to survive.

A second one only the **full** suite caught: `DataRoundTripTest.IblCache*` probes that its cache path
does **not** exist before handing it to `ScopedIBLCacheGuard` (a symlink-follow mitigation, SonarQube
cpp:S5443). `TempDir()` creates the directory, so the probe failed. `TempFile()` is the right call
there — it names a leaf inside the process-exclusive case directory *without* creating it. **If a
call site cares that the path is absent, it wants `TempFile()`, not `TempDir()`.**

**Lesson: when you replace N hand-rolled implementations with one helper, the helper has to keep
every property they had. Two of those properties here were invisible in the code being deleted — a
clean slate per test, and a path that does not exist yet — and each was found by exactly one test.
Write the helper's contracts down as tests before you trust it, and run the whole suite, not a
subset: the targeted 18,000-case hammer was green while both of these were live.**

## 6. Adding a test that writes files

1. `#include "TestTempDir.h"`, use `TempDir()` / `TempFile()`. Never `temp_directory_path()`.
2. Don't hand-roll a PID suffix — the helper's root already carries one, plus the random tag that
   survives PID reuse.
3. A resource that is **not** test-controlled (the engine's repo-relative mesh cache under
   `OloEditor/assets/cache/mesh/`) cannot be process-keyed at all. Those are serialized against each
   other with a ctest `RESOURCE_LOCK` — see [testing-architecture.md §6.2](testing-architecture.md).
4. If a case needs a path that must **not** exist, `TempFile("x")` is it: the parent is created, the
   leaf is not.
