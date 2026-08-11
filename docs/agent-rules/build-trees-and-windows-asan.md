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
refreshed, with the generated files as `BYPRODUCTS`.

**2. A `#` or `$` in a dependency path is unrepresentable in a depfile.**
The grammar escapes them (`\#`, `$$`), ninja's own depfile parser handles that
correctly — but CMake's `cmake_transform_depfile` **un-escapes them on read and
does not re-escape on write**. Ninja then splits the path at the bare `#`:

```
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

Mitigation pattern (see
`OloEngine/tests/Gameplay/ExperienceCurveTest.cpp::SerializerRejectsMalformedYAML`):
guard ONLY the throwing sub-assertion with
`#if !(OLO_ASAN_ENABLED && defined(_WIN32))` (`OLO_ASAN_ENABLED` lives in
`OloEngine/Memory/Platform.h`), keep every non-throwing assertion active,
and note which still-passing test carries the Windows-ASan coverage of
the same throw/catch plumbing. Do NOT "fix" this by changing the
malformed input or widening the catch — both were tried and both still
crash; input-shuffling is version roulette. Track state and the list of
known-vulnerable tests in issue #661.

**Expect this to bite whenever you add the FIRST test that throws through a
given entry point**, not only when you touch known-vulnerable code. The
guarded set so far:

| Test | Throwing entry point |
| --- | --- |
| `ExperienceCurveTest.SerializerRejectsMalformedYAML` | `ExperienceCurveSerializer::TestDeserializeFromYAML` |
| `CharacterClassDatabaseTest` (malformed-YAML branch) | `CharacterClassDatabaseSerializer` |
| `SceneTransitionTest.AMissingOrMalformedTargetFailsWithoutASceneAndWithAReason` | `SceneSerializer::Deserialize` → `YAML::LoadFile` (issue #642) |

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
