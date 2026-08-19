# The msvc and clangcl build trees must never run concurrently — and how to actually run Windows ASan locally

Operational knowledge from the #632 DDGI bring-up and the #661 ASan
investigation. Two independent topics that share a failure surface: the
`build/` (msvc) and `build-clang/` (clangcl / clangcl-asan) trees.

## 1. Never run the two build trees at the same time

Both trees run `GenerateBindings`, and **both write the same generated files
into the shared source tree** (`Scene/Generated/*.inl`,
`SaveGame/Generated/*.inl`, `Scripting/C#/Generated/`,
`OloEditor/src/MCP/Generated/`).

**Issue #758 narrowed this hazard but did not remove it.** Two things changed:

* The codegen is now a real `add_custom_command` with a depfile, so it runs
  only when a scanned header actually changed — not on every build. Two trees
  building at the same commit with no header edits no longer collide at all,
  because neither runs the tool.
* Each generated file is now written to a sibling temp and **renamed** into
  place. A rename is an atomic replace, so a reader sees either the complete
  old file or the complete new one. The two failures this doc used to
  describe — `CUSTOMBUILD : error : Failed to open
  "...\McpFieldRegistry.Generated.inl" for writing`, and one tree compiling a
  *half-written* `.inl` — are both gone.

What is left is an ordinary stale-output question: if a header *did* change,
both trees will want to regenerate, and whichever lands last wins. Both
compute identical content from the same source tree, so the result is correct
either way. The residual risk is timing, not corruption.

**The rule still stands** — sequence the trees. Not for the file race any
more, but for §1a below (`mspdbsrv` is per-user) and for the memory ceiling
that makes two concurrent full-width builds OOM this box.

A second, quieter cross-tree hazard: at least one vendor library
(**bc7enc**) landed a Release-flavored `.lib` where the msvc Debug link
picked it up, failing with
`LNK2038: mismatch detected for '_ITERATOR_DEBUG_LEVEL': value '0' doesn't
match value '2'` (and `RuntimeLibrary: MD_DynamicRelease` vs
`MDd_DynamicDebug`). If you see LNK2038/LNK1319 after mixing trees, the
fix is a targeted clean rebuild of the named library:
`cmake --build build --target bc7enc --config Debug --clean-first`.

**Rule: sequence the trees.** Finish (or stop) any build in one tree
before starting a build in the other. This includes background builds a
previous agent turn kicked off — check for live `MSBuild`/`ninja`/`cl`
processes first.

### 1b. Build-graph-integrating a codegen tool: four traps, all silent (issue #758)

Converting `GenerateBindings` from an `add_custom_target` (always runs) to an
`add_custom_command(OUTPUT …)` with a `DEPFILE` hit four failures that share
one shape: **no error, no warning, the build just quietly does the wrong
thing.** Every one of them was found by measuring `toolRan` across repeated
no-op builds, not by reading a log for errors. If you touch this rule, re-run
that measurement on **both** generators — a green build proves nothing here.

**1. A content-stable output cannot be the rule's OUTPUT.** The tool writes
each generated file only when its *content* changes (`WriteIfChanged`), which
is what stops a one-line header edit from recompiling the engine through
`Components.h`. But then, after a header edit, the unchanged `.inl` stays
*older* than that header — so the rule never settles and re-runs on every
build forever. Ninja survives this via `restat`; MSBuild's tlog check has no
equivalent. The fix is a **stamp** as the sole declared OUTPUT, always
refreshed.

Do not reach for `BYPRODUCTS` to name the generated files either, tempting as
it is: CMake treats byproducts as *cleanable* output, so a routine
`--target clean` deletes them — and these fifteen are **tracked in git**.
Measured in a Ninja harness: with `BYPRODUCTS`, `clean` removed all eight
`Scene/Generated` artefacts from the source tree; without it they survive.
Ordering comes from `add_dependencies(OloEngine GenerateBindings)`, not from
the outputs list, so nothing is lost by leaving them undeclared. To force a
regeneration, delete the stamp — never the generated sources.

**2. A `#` or `$` in a dependency path is unrepresentable in a depfile.**
The grammar escapes them (`\#`, `$$`), ninja's own depfile parser handles that
correctly — but CMake's `cmake_transform_depfile` **un-escapes them on read and
does not re-escape on write**. Ninja then splits the path at the bare `#`:

```text
.../Scripting/C#/ScriptGlue.h   ->   ".../Scripting/C"  +  "/ScriptGlue.h"
```

Neither fragment exists, so the edge is dirty forever. No encoding on the
emitting side avoids it (four were tried). The VS generator's
`MSBuildAdditionalInputs` transform emits a `;`-separated list and is
unaffected. `OloEngine/src/OloEngine/Scripting/C#/` is the live case here; the
tool now omits such paths (printing each one) and CMake re-adds them as
ordinary `DEPENDS`.

**3. A directory in a depfile works under Ninja and is fatal under MSBuild.**
Listing every visited directory is the classic way to catch *added or deleted*
files, because a directory's mtime moves when an entry appears or disappears.
Ninja handles it. The VS generator funnels the depfile into MSBuild's
`AdditionalInputs`, whose up-to-date check can never satisfy a directory entry
— so the rule is out of date on every build. Measured: **4.3 s per no-op build
with directories listed, 1.0 s without.** Use a `CONFIGURE_DEPENDS` glob
written to a `configure_file(... COPYONLY)` manifest instead: the manifest's
timestamp then moves exactly when the file *set* changes, on both generators.
The price is a full reconfigure when a header is added or deleted (~40 s under
VS, ~3 s under Ninja) — worth it against 4.3 s on *every* build.

**4. `CODEGEN` silently does nothing unless the layout suits it.** It is
Ninja/Makefile-only (VS ignores it), it needs `cmake_policy(SET CMP0171 NEW)`
at **top-level** scope, and — the part no doc states — it only attaches a rule
to the `codegen` target when the command's OUTPUT is consumed by a **compiled
target declared in the same directory as the `add_custom_command`**. Neither
`add_custom_target(… DEPENDS)`, nor that target's `SOURCES`, nor a
cross-directory `target_sources()` qualifies. Get any of it wrong and CMake
accepts the keyword, emits `codegen` as an *empty phony*, and
`--target codegen` exits 0 having regenerated nothing. This is why the rule
here does not use `CODEGEN`; `--target GenerateBindings` is the portable
equivalent and works under both generators.

### 1a. `mspdbsrv` is per-USER, not per-worktree — one wedged instance stalls every build on the box

Found the hard way during #636, with three worktrees live. Symptoms, in
order of appearance:

1. A vendor project fails repeatedly with
   `error C1041: cannot open program database '…libprotocd.pdb'; if
   multiple CL.EXE write to the same .PDB file, please use /FS`. Each retry
   advances a few source files, then fails again on a different one.
   `--parallel 1`, `/p:CL_MPCount=1` and `CL=/FS` all fail to fix it — which
   is the tell that this is **not** the documented concurrency case.
2. The `.pdb` cannot be deleted: "used by another process". The holders are
   idle MSBuild **node-reuse** workers, which keep file handles alive for
   ~15 minutes after their build ends.
3. Later, compilation across the whole machine simply stops. `cl.exe`
   processes sit for **hours** having burned only seconds of CPU — no
   progress, no error, no output. A build that looks slow is actually dead.

The shared resource is `mspdbsrv.exe`: one instance per user session,
serialising `/Zi` PDB writes for **every** compiler on the machine,
regardless of which worktree or build tree it belongs to. When it wedges,
every `/Zi` compile in every worktree blocks on it indefinitely.

Diagnosis — the decisive signal is **CPU time, not wall time**:

```powershell
Get-Process cl | Select-Object Id, StartTime, @{n='CPU_s';e={[math]::Round($_.CPU)}}
```

Hours of wall-clock against seconds of CPU means blocked, not busy. To find
out whose build a stalled `cl.exe` belongs to (it is often not yours — the
command line is just an `@…rsp` path), read the response file:

```powershell
Get-CimInstance Win32_Process -Filter "Name='cl.exe'" |
  ForEach-Object { $_.CommandLine }   # -> @C:\...\tmpXXXX.rsp
# then grep the .rsp for the source paths it lists
```

Recovery, in order:

1. `Stop-Process -Name mspdbsrv -Force` — it is a service, and restarts on
   demand. This alone unblocks a merely-contended machine.
2. If `cl.exe` processes survive with flat CPU, they are pointed at the dead
   pipe and will never finish: kill them too.
3. `Stop-Process -Name MSBuild -Force` to drop idle node-reuse workers that
   still hold `.pdb` handles, then delete the offending `.pdb`.
4. Re-run the build. Object files persist, so it resumes rather than
   restarting.

**Before killing anything, verify no compiler is genuinely working** (a
process with climbing CPU), because these processes are shared across every
worktree on the box. Prevention: set `MSBUILDDISABLENODEREUSE=1` for agent
builds so workers exit with the build instead of lingering on file handles.

## 2. Building and running the `clangcl-asan` preset locally

**Before #697, this whole section was unrunnable locally.** `clangcl-asan`
inherits `clangcl`, and with the default `OLO_WITH_USD=ON` neither preset
could reach a successful link: the engine force-linked OpenUSD with a bare
`/WHOLEARCHIVE:usd_m`, which `lld-link` (unlike `link.exe`) will not resolve
against `/LIBPATH`, so every executable died with
`lld-link: error: could not open 'usd_m': no such file or directory`. The
workaround was to configure with `-DOLO_WITH_USD=OFF`; that is **no longer
needed** — the option now carries the full path and links under both linkers.
If you see a bare-name `/WHOLEARCHIVE` failure again, see gotcha 7 in
[asset-import-usd-alembic.md](asset-import-usd-alembic.md) before deleting
the option (deleting it moves the failure to runtime).

The CI job (`.github/workflows/asan.yml`, "ASan (Windows/clang-cl)") works
because of three non-obvious choices you must replicate locally:

- **Build `--config Release`, never Debug.** clang-cl refuses ASan with
  the debug CRT outright:
  `clang-cl: error: invalid argument '-MDd' not allowed with ...`
  ("AddressSanitizer doesn't support linking with debug runtime libraries
  yet"). The CI build/ctest steps select Release; do the same:
  `cmake --preset clangcl-asan` then
  `cmake --build build-clang --target OloEngine-Tests --config Release --parallel 6`.
  (Bare `--parallel` forwards the omission to Ninja, whose default here is
  `cores + 2` = 18 — see CLAUDE.md; always give it a number.)
- **Put the LLVM-matched ASan runtime directory on PATH** before running
  anything ASan-instrumented (including `OloHeaderTool.exe`, which the
  build itself executes):
  `C:\Program Files\LLVM\lib\clang\<version>\lib\windows`.
  Without it the binary dies silently with exit `0xC0000135`
  (DLL not found). The **MSVC-bundled** ASan runtime
  (`VC\Tools\MSVC\...\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll`)
  is NOT interchangeable — standalone-LLVM-built binaries fail against it
  with `0xC0000139` (entry point not found). Match the runtime to the
  compiler in `build-clang/CMakeCache.txt` (`CMAKE_CXX_COMPILER`).
  (`0xC0000135` has a second, unrelated cause on this repo: a missing
  `steam_api64.dll` next to the executable — see
  [configure-time-variable-visibility.md](configure-time-variable-visibility.md).
  Tell them apart with `dumpbin /DEPENDENTS` on the exe.)
- **To see an ASan crash report instead of gtest's opaque
  `SEH exception with code 0xc0000005 thrown in the test body`**, run the
  failing test with `--gtest_catch_exceptions=0`. gtest's SEH catcher
  otherwise intercepts the fault before ASan's own SEGV reporter can
  print the stack.

## 2b. A green msvc build is NOT evidence the sanitizer CI will compile

Every sanitizer job — the three Linux ones *and* Windows clang-cl — builds with
Clang, which is stricter than MSVC's default conformance. A branch can be fully
green locally (build + 5000-test suite + live editor verification) and still fail
**all four** jobs at the compile step, on one error.

The case that produced this note (issue #607, PR #687): a **default argument
using a nested class's default member initializers**.

```cpp
class GPUResourceInspector {
    struct CaptureRegion { u32 X = 0; /* … */ };                 // NSDMIs
    static Result CaptureTexturePng(/*…*/, CaptureRegion r = {}); // ← Clang: error
};
```

> `error: default member initializer for 'X' needed within definition of
> enclosing class 'GPUResourceInspector' outside of member functions`

A nested class's NSDMIs are not usable from the *enclosing* class's
complete-class context while that class is still incomplete
(`[class.mem.general]`). MSVC accepts it; Clang is right to refuse. The fix is to
define the struct at **namespace scope** and alias it back in, which keeps every
`Outer::CaptureRegion{…}` call site spelled the same:

```cpp
struct GPUCaptureRegion { u32 X = 0; /* … */ };
class GPUResourceInspector { using CaptureRegion = GPUCaptureRegion; /* … */ };
```

**Cheap pre-push check — seconds, no second build tree.** A full `clangcl`
configure+build is a cold vendor build (~1 h), so it is not a practical gate for
a one-file change. Syntax-only compile the touched TUs instead, reusing the msvc
tree's own include dirs:

```bash
# Union the include dirs CMake already computed for the target.
grep -o '<AdditionalIncludeDirectories>[^<]*' build/OloEngine/src/OloEngine.vcxproj \
  | head -1 | sed 's|<AdditionalIncludeDirectories>||' | tr ';' '\n' \
  | grep -v '%(' | grep -v '^$' > /tmp/incs.txt

export MSYS2_ARG_CONV_EXCL='*'          # or Git Bash mangles /std:, /I, /EHsc
INC=$(sed 's|^|/I|' /tmp/incs.txt | tr '\n' ' ')
"/c/Program Files/LLVM/bin/clang-cl.exe" /std:c++latest /EHsc $INC \
    /clang:-fsyntax-only /clang:-w /TP /c 'C:\path\to\Changed.cpp'
```

Two traps: pass the source as a **Windows** path (clang-cl can't open `/tmp/x.cpp`),
and use the **tests** project's include dirs (`build/OloEngine/tests/…vcxproj`,
plus `OloEngine/vendor/googletest-src/{googletest,googlemock}/include`) when
checking a test or an `OloEditor/src/MCP/*.cpp` TU — the sanitizer jobs build the
`OloEngine-Tests` target, which compiles the MCP editor sources too.

**Confirm the check is load-bearing**: `git stash` the fix, re-run, and verify it
reproduces the CI error at the same line. A syntax-only check that silently
skipped the header would otherwise "pass" for any change at all.

## 3. A fresh worktree's first `cmake --preset msvc` can wedge on the GameNetworkingSockets/WebRTC vendor clone

Each git worktree gets its own independent `OloEngine/vendor/` — nothing is
shared across worktrees — so a worktree that has never been configured pays
the full vendor bootstrap, and `gamenetworkingsockets-src` (which vendors a
WebRTC submodule tree, ~700 MB / ~9k files) is by far the heaviest of the
CPM/FetchContent dependencies. If a previous configure attempt in that same
worktree was interrupted (terminal closed, agent turn ended mid-build), git's
submodule clone can leave orphaned `git.exe` child processes still crawling
the WebRTC submodules in the background — invisible to whatever killed the
parent build. The *next* configure then fails fast with
`Error removing directory ".../gamenetworkingsockets-src". Failed to remove
directory` / `CMake Error ... FetchContent.cmake ... Build step for
gamenetworkingsockets failed`, because FetchContent tries to wipe and
re-populate the directory while those orphaned processes still hold file
handles inside it (`.git/modules/.../objects/pack/tmp_pack_*`).

Diagnose with `tasklist /FI "IMAGENAME eq git.exe"` (native `cmd`/PowerShell;
`tasklist | grep -i git.exe` works too but needs Git Bash) — dozens of
small-footprint processes is the signature — before assuming the vendor
mirror is broken —
letting them finish, or waiting a few minutes and retrying, is one fix. Far
faster if a sibling worktree already has a fully-populated
`gamenetworkingsockets-src` (check for one under `<other-worktree>/OloEngine/
vendor/`, e.g. from `resume-worktrees`/`start-work`'s registry): mirror-copy
it instead of re-cloning from GitHub —
`robocopy <sibling>\OloEngine\vendor\gamenetworkingsockets-src
<this-worktree>\OloEngine\vendor\gamenetworkingsockets-src /MIR /MT:16` (a
plain directory copy of an already-checked-out repo at the same pin; nothing
worktree-specific lives inside it) — then delete the stale
`gamenetworkingsockets-subbuild`/`-build` dirs so CMake regenerates them
against the copied source, and reconfigure. Note **robocopy's exit code is a
bitmask, not a plain 0-means-success signal**: codes **0-7** are all
non-failure (0 = nothing to copy, 1 = files copied successfully, 2/4 = extra/
mismatched files noted — any OR-combination of those three bits is still
fine), while **8 or higher** sets the "some files/dirs could not be copied" or
"serious error" bits and means stop and investigate. Don't read a nonzero
robocopy exit code as an error the way you would for every other tool — check
the actual value against that threshold instead.

## 4. Known toolchain bug: C++ throws through instrumented frames can AV (issue #661)

clang-cl + `/fsanitize=address` on Windows crashes **inside the C++
exception-dispatch machinery** (access-violation reading near null, with
`0x19930520` — the MSVC C++ throw magic — in the registers, before any
catch clause runs) when an exception is thrown through certain
sanitizer-instrumented frame shapes. In this codebase the trigger is
yaml-cpp throwing `ParserException` on malformed input, but the input
shape and the catch type are both irrelevant (experimentally eliminated),
and which call paths crash is frame-layout and clang-version dependent.

### The guard: `OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN`

**Do not hand-roll `#if !(OLO_ASAN_ENABLED && defined(_WIN32))` any more.** The
fourth occurrence (#458, guarded in PR #802) was the point at which #661's own
follow-up list said to centralise, so the guard now lives in
`OloEngine/tests/WinAsanYamlThrow.h`, which owns the macro *and* the single
evidence trail — the "what does SEH 0xc0000005 with no ASan report and no stack
mean" explanation is written down once, in the header, rather than re-derived
each time. Include it and guard **only** the sub-assertion that makes the
third-party library throw:

```cpp
#include "WinAsanYamlThrow.h"
...
#if !OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN
    EXPECT_FALSE(Parse("key: [unclosed"));      // the throwing case
#endif
    EXPECT_FALSE(Parse("WrongRootKey: 1\n"));   // ours, always runs
```

Keep every non-throwing assertion active, and note which still-passing test
carries the Windows-ASan coverage of the same throw/catch plumbing. Do NOT
"fix" this by changing the malformed input or widening the catch — both were
tried and both still crash; input-shuffling is version roulette. Track state and
the list of known-vulnerable tests in issue #661; the header carries the
retirement criteria (delete it and every use once the upstream LLVM fix lands).

**Both polarities are in the tree, deliberately — read the sign before copying.**
`#if !OLO_SKIP_…` wraps a branch that is simply *dropped* under the guard;
`#if OLO_SKIP_…` selects a **substitute** non-throwing branch. Which one you want
depends on the next trap:

> **A skipped throw can leave the rest of the test vacuously true.** In
> `AccessibilitySettingsTest` the assertion after the load is "the previous
> settings survived" — but the test *set* those settings two lines earlier, so
> skipping the load outright reduces it to re-reading the value it just wrote:
> green, and proving nothing. The fix is to substitute a **well-formed file with
> the wrong root key**, which takes the same `LoadFromFile` rejection path
> *without* throwing, so the preservation assertion still means something under
> ASan. Before you guard, check what the surviving assertions are still resting
> on.

**Expect this to bite whenever you add the FIRST test that throws through a
given entry point**, not only when you touch known-vulnerable code. The
guarded set so far:

| Test | Throwing entry point | Guard |
| --- | --- | --- |
| `ExperienceCurveTest.SerializerRejectsMalformedYAML` | `ExperienceCurveSerializer::TestDeserializeFromYAML` | helper |
| `CharacterClassDatabaseTest` (malformed-YAML branch) | `CharacterClassDatabaseSerializer` | helper |
| `AccessibilitySettingsTest.LoadOfACorruptFileFailsWithoutCorruptingSettings` | `Accessibility::LoadFromFile` → `YAML::LoadFile` (issue #458) | helper (substitute branch) |
| `SceneTransitionTest.AMissingOrMalformedTargetFailsWithoutASceneAndWithAReason` | `SceneSerializer::Deserialize` → `YAML::LoadFile` (issue #642) | **still hand-rolled** |

The `SceneTransitionTest` row is the one straggler: PR #802 migrated the two
`Gameplay/` sites onto the helper but left this one on the raw
`#if !(OLO_ASAN_ENABLED && defined(_WIN32))`. It is not broken — the condition is
identical — but it is the copy a future author is most likely to find and clone.
Migrate it the next time that file is touched.

Note that not every `OLO_ASAN_ENABLED` in the test tree belongs to this bug:
`ServerAuthoritativeLoopTest` guards on it for the unrelated
GameNetworkingSockets stack-buffer-overflow (issue #317). Grepping the macro
over-reports the guarded set; grep `OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN` instead.

The #642 case is instructive about how the bug hides: the suite already had
`SceneSerializerFuzzRegressionTest` firing a dozen malformed payloads at
`SceneSerializer::DeserializeFromYAML` under ASan without trouble — because
every one of those inputs **parses** cleanly and fails later in the schema
walk, so yaml-cpp never throws. The first genuinely *unparseable* bytes handed
to the sibling file-path overload crashed immediately. So "a nearby
malformed-input test already passes under ASan" is not evidence your new one
will: check whether the existing inputs actually reach a `throw`.

A corollary for writing the guard comment: don't claim a same-plumbing sibling
carries the coverage unless one really does. For `SceneSerializer::Deserialize`
none does — nothing else in the suite throws through it — so the honest note is
that the file-path throw is uncovered under Windows ASan while the
non-throwing branches of the same test stay active everywhere.

## 5. Instrumenting a build, and a per-file-set compile job pool (issues #759, #822)

### 5a. Use the native recipe (CMake 4.3+), not the manual gate below

As of #822 this machine is on CMake 4.4.2. The repo's own `cmake_minimum_required` floor
is unchanged (still `3.25` in the root `CMakeLists.txt`, and the presets' `cmakeMinimumRequired`
is `4.2.0` — see the hard constraints in issue #822, not raised by this work); the
instrumentation and heavy-compile-pool blocks below are purely additive, each behind its own
`if(CMAKE_VERSION VERSION_GREATER_EQUAL ...)` guard, and no-op cleanly on an older CMake.
`cmake_instrumentation()` — the API described in §5b below, now stable — is wired
directly into the root `CMakeLists.txt`, gated `if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.3)`.
No UUID gate, no manually-written query file:

```powershell
cmake --preset clangcl -DOLO_BUILD_INSTRUMENTATION=ON
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-clang --target OloEngine-Tests --config Debug --parallel 6'
```

`OLO_BUILD_INSTRUMENTATION` defaults `OFF` (the launcher wraps every compile/link/custom
command, and profiling every developer build by default is not wanted). It has no effect under
any generator the API itself doesn't support — Makefile/Ninja/FASTBuild only (§5b point 1 still
applies), checked as an **allow-list** in the root `CMakeLists.txt`, not a deny-list on just
"Visual Studio". That matters for the primary `msvc` preset (`build/`, Visual Studio 18 2026,
this repo's actual unsupported case) **and** for Xcode or any other generator this repo doesn't
currently use — every one of them prints a `message(WARNING)` naming the actual generator in use,
rather than silently attempting instrumentation and doing nothing.

**Verify it wired** the same way as before trusting a build: `build-clang/CMakeFiles/rules.ninja`
compile/link rules must carry the `"…/ctest.exe" --instrument --command-type compile …`
prefix, and `build-clang/.cmake/instrumentation/v1/query/generated/query-0.json` must exist
after configure — no UUID suffix on the directory name this time, that was only ever an
experimental-gate artifact (§5b). The trace lands at
`build-clang/.cmake/instrumentation/v1/data/trace/trace-<timestamp>.json` after the build
completes (the `postCMakeBuild` hook). **Only the most recent trace file is kept** — a second
build's indexing deletes the first trace, so copy out anything you want to keep before
re-running.

A minimal analysis script (per-TU compile time ranked descending, peak host memory, role
totals) is the fastest way to read a multi-MB trace; see the `#822` PR for one, or write
~40 lines against the JSON directly — each event's `args.role` is `compile`/`link`/`custom`/
`cmakeBuild`, `args.source` is the TU path for compile events, `dur` is microseconds, and
`args.dynamicSystemInformation.afterHostMemoryUsed` is **system-wide** host memory in KiB
(not the command's own RSS) sampled at that command's completion.

### 5b. Historical: the pre-4.3 experimental gate (only relevant on CMake < 4.3)

CMake 4.x ships an experimental Instrumentation API that emits a Google Trace
Event file with per-command timing, target attribution and per-command
**host-memory** samples. Getting it to actually emit anything has two
non-obvious gates, both of which fail *silently* (a green configure, no trace):

1. **It does not work under the Visual Studio generator.** The feature is
   Makefile / Ninja / FASTBuild only (`cmake-instrumentation(7)`). So the
   primary `build/` (VS 18 2026) tree **cannot** be instrumented at all — use
   the `build-clang/` Ninja tree (`cmake --preset clangcl`). That is also the
   only tree where `OLO_LINK_JOBS` exists (it is a Ninja job pool), so it is the
   correct tree for any link-concurrency question regardless.
2. **While experimental, the query directory name carries the gate UUID.** The
   shipped manual (`cmake-instrumentation.7.rst`) says query files go under
   `<build>/.cmake/instrumentation/v1/query/` — that path is **ignored**. The
   experimental gate requires the UUID **appended to the directory name**:
   `<build>/.cmake/instrumentation-ec7aa2dc-b87f-45a3-8022-fe01c5f59984/v1/query/`.
   With the plain `instrumentation/` path, configure prints
   `Manually-specified variables were not used by the project:
   CMAKE_EXPERIMENTAL_INSTRUMENTATION` and wires no launcher. The UUID-suffixed
   rule is documented only in `Help/dev/experimental.rst`, which is **not shipped
   in the binary install** — read it from the release tag on gitlab.kitware.com.

Recipe that works on CMake 4.2:

```powershell
$GATE = "ec7aa2dc-b87f-45a3-8022-fe01c5f59984"   # this exact UUID; 4.2-specific
$q = "build-clang/.cmake/instrumentation-$GATE/v1/query"
New-Item -ItemType Directory -Force $q | Out-Null
'{ "version":1, "hooks":["postGenerate","postCMakeBuild"],
   "options":["staticSystemInformation","dynamicSystemInformation","trace"] }' |
   Set-Content "$q/olo.json"
cmake --preset clangcl -DCMAKE_EXPERIMENTAL_INSTRUMENTATION=$GATE
cmake --build build-clang --config Debug --parallel 6     # must be `cmake --build` for the hook
```

**Verify it wired** before trusting a build: the compile rule in
`build-clang/CMakeFiles/rules.ninja` must gain a
`"…/ctest.exe" --instrument --command-type compile … -- <compiler>` prefix, and
`…/instrumentation-<UUID>/v1/data/` must appear after configure. The trace lands
at `…/instrumentation-<UUID>/v1/data/trace/trace-*.json` (one event per
compile/link/custom command; `args.dynamicSystemInformation.afterHostMemoryUsed`
is **system-wide** host memory in KiB, not the command's RSS; the top-level
`dur`/`ts` are microseconds). `--gtest`-style grepping for "instrument" in
`build.ninja` is useless here — the worktree path itself
(`OloEngine-build-instrumentation-759`) contains the substring.

### 5c. What #759 measured (clean Debug build, `build-clang/`, clang-cl, this host)

Host was an i7-14700KF (20 cores / 28 threads, 64 GB), CI runners idle — **not**
the 16c/31GB CLAUDE.md assumes; confirm your host before reusing these numbers.
Ports build at *configure* time (vcpkg) and are absent from the build trace, so
these are OloEngine + small in-tree vendor + FFmpeg only:

- Cold configure+generate **796 s** (dominated by the cold vcpkg install of 45
  packages; warm reconfigure ~11 s). Build step at **-j6: 607 s**, at **-j12:
  382 s** (1.59×).
- Composition (-j6): **compile is 96.5 % of build CPU-work** (2569 CPU-s over
  1314 TUs) vs 89 CPU-s of linking over 13 links. Worst single TUs:
  `LuaScriptGlue.cpp` ~150 s, `McpFieldRegistry.cpp` ~52 s (compiled in both
  OloEditor and OloEngine-Tests). Top targets: OloEngine-Tests (612 TU),
  OloEngine (589 TU), OloEditor (55 TU); OloRuntime/OloServer are ~3 TU each.
- **The `OLO_LINK_JOBS=2` pool is NOT the serialization point — compilation is.**
  Links are off the critical path: the build ends compile-bound (OloEditor's
  last TU finishes, then one ~10-17 s `OloEditor.exe` link). The pool only
  briefly saturates when OloRuntime+OloServer link together; Tests then links
  with ~0 s slot wait. Raising `OLO_LINK_JOBS` buys ~0 wall-time on a full build,
  so keep it at 2 — it is zero-cost insurance against the link-memory spike on a
  smaller/shared host.
- **Peak host memory barely scales with -j** because a handful of heavy TUs set
  the peak, not the lane count. clang-cl: **41.6 GiB at -j6, 43.6 GiB at -j12
  (+2.0 GiB for 2× width)**. A Ninja+**MSVC** tree stood up for the same
  measurement (real `cl.exe`/`link.exe` — the memory-hungrier toolchain the
  folklore OOM was on): **45.7 GiB at -j6, 47.4 GiB at -j12 (+1.7 GiB)**, still
  ~16 GiB headroom, wall 1019 s → 776 s (1.31×). Peak is a *compile* on both
  toolchains (the pool caps links at 2). So on this host the `-j6` guidance is a
  safety *floor* for the documented 16c/31GB + concurrent-runner worst case, not
  a memory limit that binds here. (To instrument the MSVC compiler you need a
  Ninja tree with cl.exe — configure/build from inside a `vcvars64.bat` shell,
  since the VS-generator tree itself is uninstrumentable.)
- `CMAKE_OPTIMIZE_DEPENDENCIES=ON`: clean-build wall delta **+0.6 s (noise)**.
  It roughly halved link CPU-time (89→44 s, plausibly pruned transitive link
  libs) but that is fully hidden under the compile-bound critical path — no
  wall-clock benefit for this graph. `INSTALL_PARALLEL` is N/A (we never install).

### 5d. The heavy-compile file-set pool (CMake 4.4, issue #822)

A fresh trace taken five days after #759, on the same host, confirms the same two
outliers **by identity** (only their size grew with the intervening work):
`LuaScriptGlue.cpp` (145–198 s across repeated runs) and `McpFieldRegistry.cpp`
(40–67 s, compiled separately into both `OloEditor` and `OloEngine-Tests` — see
`docs/agent-rules/component-serializer-codegen.md`-adjacent MCP field-registry
context). Peak host memory at `-j6` (narrow scope, `OloEngine-Tests` only) came in
at **41.4 GiB**, within noise of #759's 41.6 GiB — a solid corroboration that
nothing structural drifted between the two measurements.

CMake 4.4's `SOURCES`-type file set can carry its own `JOB_POOL_COMPILE`, which
overrides the target/source-level setting. #822 pulls the two heavy TUs (three
compile sites: `OloEngine`'s `LuaScriptGlue.cpp`, `OloEditor`'s and
`OloEngine-Tests`'s independent copies of `McpFieldRegistry.cpp`) out of their
targets' plain source lists and rebinds them via a file set into a new
Ninja-only `olo_heavy` pool, default depth 2 (`-DOLO_HEAVY_COMPILE_JOBS=<N>` to
change it), validated with the same "reject 0/empty" rigor as `OLO_LINK_JOBS`.
`cmake/CommonProperties.cmake`'s `olo_bind_heavy_compile_pool()` centralizes the
file-set + pool-property binding so each of the three call sites is one line.

**Two real bugs found while wiring this — both would have been silent
misconfigurations, not build failures, without the verification the plan
demanded:**

1. **The `JOB_POOL_COMPILE` file-set property's own CMake 4.4 doc example is
   wrong.** `set_property(FILE_SET my_fileset TYPE SOURCE PROPERTY
   JOB_POOL_COMPILE two_jobs)` — copied verbatim from `Help/prop_fs/JOB_POOL_COMPILE.rst`
   — errors `set_property required TARGET option is missing`, because
   `set_property()`'s actual `FILE_SET` scope signature (`Help/command/set_property.rst`)
   is `FILE_SET <file_set>... TARGET <target>`, no `TYPE` keyword at all. Caught
   by testing the exact snippet in a throwaway two-source scratch project against
   a real generated `build.ninja` *before* touching the 1300-TU tree — cheap
   insurance that would have cost a full reconfigure-and-grep cycle on the real
   tree to catch otherwise.
2. **`target_sources(... FILE_SET ...)`'s relative `FILES` resolve against the
   *caller's* `CMAKE_CURRENT_SOURCE_DIR`, not the function's `base_dir`
   argument.** `olo_bind_heavy_compile_pool()`'s first version passed relative
   paths straight through to `target_sources()`; called from
   `OloEngine/tests/CMakeLists.txt` with `base_dir` pointing at `OloEditor/src`,
   CMake looked for the file under `OloEngine/tests/` instead — a **hard
   configure error** ("must be in one of the file set's base directories"), not
   a silent drop, but only because the file happened to not exist at the wrong
   location; a same-named collision could have resolved to the wrong file
   instead of erroring. Fixed by resolving `FILES` against `base_dir` explicitly
   inside the function (`IS_ABSOLUTE` check, then join) rather than trusting
   `target_sources()`'s own relative-path handling.

**Measurement methodology note, also load-bearing:** the plan's literal
instruction ("clean Debug build of `OloEngine-Tests`") under-scopes the
experiment. `OloEngine-Tests` does not link against `OloEditor` — it recompiles
`OloEditor`'s `.cpp` files directly as its own sources (see
`OloEngine/tests/CMakeLists.txt`'s comment on this) — so `--target OloEngine-Tests`
alone never builds `OloEditor.exe`, and therefore never compiles *`OloEditor`'s own
copy* of `McpFieldRegistry.cpp`. At that narrow scope only two heavy compiles
exist at all (`LuaScriptGlue.cpp` + one `McpFieldRegistry.cpp`), so a depth-2 pool
and a depth-6 (~unthrottled) pool are behaviorally indistinguishable — both
already allow every heavy TU that scope can produce to run unblocked. The
comparison that actually exercises the pool needs `--target OloEditor --target
OloEngine-Tests` together, which is where the three-heavy-TU-instance scenario
the pool is meant to arbitrate genuinely exists.

**Results, `OloEditor` + `OloEngine-Tests` together, `-j6`, clean, clang-cl,
idle host, `OLO_BUILD_INSTRUMENTATION=ON`:**

| pool depth | wall time | peak host memory |
|---|---|---|
| 2 (real default) — run 1 | 583.4 s | 38.42 GiB |
| 2 (real default) — run 2 | 576.8 s | 38.79 GiB |
| 6 (~unthrottled) — run 1 | 624.8 s | 40.48 GiB |

Depth 2 is **~7 % faster** and uses **~4–5 % less peak memory** than depth 6,
fairly consistently across the two repeated depth-2 runs (within 1.2 % of each
other). **Honest caveat, stated plainly rather than oversold:** a direct overlap
check of the three heavy TUs' `[timeStart, timeStart+dur]` windows in both traces
found they almost never directly overlap **with each other** regardless of pool
depth (max concurrent 1–2 either way) — so the improvement is *not* cleanly
attributable to "the pool stopped two heavy TUs racing each other," the
mechanism's most intuitive story. Total system-wide concurrent build-step count
never exceeded 6 in either configuration (confirms a named Ninja pool is a
subsidiary cap *within* the `-j` budget, not additional capacity on top of it —
worth confirming empirically once, since it is easy to reason about backwards).
The likely explanation is a second-order scheduling effect — constraining the
heavy pool changes *when* those TUs start relative to the rest of the 1300+-TU
graph, which shifts what else is running concurrently at the moment each heavy
TU peaks. Sample size is small (n=2 vs n=1); do not read more precision into the
percentages than that supports.

**Decision:** ship the pool at its structurally-conservative default (depth 2),
same framing #759 used for `OLO_LINK_JOBS` itself — cheap, verified-real
insurance against the documented worst case, not a headline win. The `-j6`
compile-width guidance in `CLAUDE.md` is **not** revisited by this data; the
measured deltas here are too modest and the sample too small to move that
needle in either direction.

### 5e. Both memory pools are Ninja-only — so on Linux CI, `--parallel N` is the *only* cap (issue #796)

`OLO_LINK_JOBS` (§5c) and `olo_heavy` / `OLO_HEAVY_COMPILE_JOBS` (§5d) are Ninja
job pools, and the root `CMakeLists.txt` guards both with
`if(CMAKE_GENERATOR MATCHES "Ninja")`. §5b already notes this for the local **VS**
tree. The consequence nobody had written down is about **CI**: every Linux job in
this repo configures with no `-G`, so it gets **Unix Makefiles**, so *both* pools
are silently inert there. Nothing warns — the pools simply do not exist in the
generated makefiles.

Two things follow, and the first one cost this repo ten weeks of red CI.

1. **A bare `--parallel` means the opposite thing per generator, and only the
   Makefiles case is unbounded.** CMake does not pick a number for an
   argument-less `--parallel`; it forwards the *omission* to the native tool and
   takes that tool's default:

   | generator | native tool | bare `--parallel` resolves to |
   |---|---|---|
   | Unix Makefiles | `make` | `-j` with **no number — unlimited** |
   | Visual Studio | MSBuild | `/m` = core count (4 on a hosted runner) |
   | Ninja | `ninja` | cores + 2 |

   Only the first is a runaway. That is why `video-ffmpeg.yml`'s Linux leg
   OOM-killed the runner at ~29 % — still inside vendor deps, before a single
   engine source file — on every weekly run for 10+ weeks (#796), while the three
   *other* bare-`--parallel` sites in `.github/workflows/` sat green the whole
   time: all three are `windows-latest`, where the tool default is bounded.
   **Auditing for the string `--parallel` is therefore not the check — the check
   is `--parallel` *plus* the job's generator.**

2. **On a Linux runner, the `-jN` you pass is the entire memory story.** There is
   no link pool serialising the static-engine link and no `olo_heavy` pool
   throttling `LuaScriptGlue.cpp` / `McpFieldRegistry.cpp`. Pick the number
   against evidence rather than against the pools you think are protecting you.
   The proven-green reference points on `ubuntu-24.04` are `steam-stub.yml`
   (`--parallel 4`, `OloEngine-Tests`, Debug, clang-19 + lld) and `asan.yml`'s
   sanitizer jobs (`--parallel 2`, where each TU costs ~3 GB).

**When you add a Linux CI job, copy `steam-stub.yml`, not a Windows job.** It
carries the whole set in one place: `uses: ./.github/actions/setup-llvm-apt` plus
`clang-19 lld-19` in the apt list (the default libstdc++ lacks
`std::forward_like`, which `Core/Reflection/MemberList.h` uses),
`-DCMAKE_{C,CXX}_COMPILER=clang(++)-19`, `-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld`
(BFD `ld` can exceed a 16 GB runner on this link), `CMAKE_BUILD_TYPE` at configure
time (`--config` is *silently ignored* by the single-config generator — it will
happily lie to you about building Release), and an explicit `--parallel N`.

**Never hand-roll the apt.llvm.org step** — use the action. Five jobs had grown
byte-identical copies of it, and all five shared two defects nobody noticed until
a review on #827: the key went into `/etc/apt/trusted.gpg.d` (trusted for *every*
repository on the runner, not just LLVM's) and nothing checked *which* key had
arrived, because the fetch ran under `bash -e` with no `pipefail` and so reported
`tee`'s status rather than `wget`'s. The action now scopes the key with
`signed-by=` and pins its primary-key fingerprint. That is also the reason it is
an action rather than a fifth corrected copy: one `workflow_dispatch` run that
exercises it is evidence for every call site.
