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

The CI job (`.github/workflows/asan.yml`, "ASan (Windows/clang-cl)") works
because of three non-obvious choices you must replicate locally:

- **Build `--config Release`, never Debug.** clang-cl refuses ASan with
  the debug CRT outright:
  `clang-cl: error: invalid argument '-MDd' not allowed with ...`
  ("AddressSanitizer doesn't support linking with debug runtime libraries
  yet"). The CI build/ctest steps select Release; do the same:
  `cmake --preset clangcl-asan` then
  `cmake --build build-clang --target OloEngine-Tests --config Release --parallel`.
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

## 3. Known toolchain bug: C++ throws through instrumented frames can AV (issue #661)

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
