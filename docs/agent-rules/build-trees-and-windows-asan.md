# The msvc and clangcl build trees must never run concurrently — and how to actually run Windows ASan locally

Operational knowledge from the #632 DDGI bring-up and the #661 ASan
investigation. Two independent topics that share a failure surface: the
`build/` (msvc) and `build-clang/` (clangcl / clangcl-asan) trees.

## 1. Never run the two build trees at the same time

Both trees run the `GenerateBindings` custom target, and **both write the
same generated files into the shared source tree**
(`Scene/Generated/*.inl`, `SaveGame/Generated/*.inl`,
`Scripting/C#/Generated/`, `OloEditor/src/MCP/Generated/`). Two concurrent
builds race on those files and fail with
`CUSTOMBUILD : error : Failed to open "...\McpFieldRegistry.Generated.inl"
for writing` — or worse, one tree compiles a half-written `.inl`.

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
