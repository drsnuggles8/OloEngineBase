# Intermediate build outputs belong to one build tree

Write every intermediate a build consumes (static archives, generated headers) under
`${CMAKE_BINARY_DIR}`, never under `${CMAKE_SOURCE_DIR}`. Two build trees in one worktree (a
`build-cached` and a `build-cached-asan`, or the GHASH-on and GHASH-off trees of a measurement)
otherwise read each other's files, and nothing reports it (#1386).

## What went wrong

Until #1386 these intermediates lived in the source tree:

| output | path | what a second tree did to it |
|---|---|---|
| engine archives | `bin/<Config>/OloEngine*/OloEngine*.lib` | overwrote them; the first tree's next link used the second tree's archive |
| vendored static libs | `OloEngine/vendor/{imgui,imguizmo,lua,bc7enc,xatlas}-build/` | same |
| glad's generated loader | `OloEngine/vendor/glad-build/` | its generate rule runs `remove_directory` first, so a sibling compile failed |
| copied stb headers, LuaScriptCore's import library | `OloEngine/vendor/stb_image-build/`, `OloEditor/Resources/Scripts/` | same files rewritten by every tree |

The glad case is loud. A fresh tree's first build deleted `<glad/gl.h>` while another tree in the
same worktree was compiling, and that build stopped with
`fatal error: 'KHR/khrplatform.h' file not found` in a test that had not changed.

The archive case is silent. Ninja decides by timestamps, and the overwritten archive is newer than
the first tree's objects, so that tree considers it up to date and links it. During the #1386
measurements the GHASH-on Debug build replaced the GHASH-off tree's three engine archives; the
`.debug$H` sections in the "off" input were the only sign. An ASan Release tree likewise feeds
instrumented archives to a plain Release tree.

`build-lock.ps1` makes the race likely rather than rare: it admits two cached trees concurrently,
and it does not distinguish two worktrees from two trees of one worktree.

## The rule

- Static archives go to `${CMAKE_BINARY_DIR}/lib/<Config>/<target>/`
  (`olo_set_output_directories` in [CommonProperties.cmake](../../cmake/CommonProperties.cmake)).
  The editor's automation table in `AutomationBuildInvocation.h` reports the engine archive as
  build-directory relative for that reason.
- Vendored libraries, glad's generated loader and the copied stb headers go under the vendor
  directory's binary dir. `OloEngine-LuaScriptCore`'s import library goes to `<build>/lib/`; its
  DLL stays in `OloEditor/Resources/Scripts/` with the other script binaries.
- Executables and DLLs stay in `bin/<Config>/<target>/`. Scripts, the VS Code tasks and the
  smoke tests run them from there, so the last tree to link one wins. Treat a `bin/` binary as
  belonging to whichever tree linked it last, and relink from the tree you mean before using it
  as evidence.

Still shared on purpose, and why that is safe:

- FetchContent sources and sub-builds under `OloEngine/vendor/clang/` (`FETCHCONTENT_BASE_DIR`
  of every clang-cl preset). They change only at configure time, so configure one tree at a time
  ([concurrent-cmake-configure.md](concurrent-cmake-configure.md)).
- The tracked generated sources `GenerateBindings` writes. Every tree computes the same content
  and each file is replaced by an atomic rename
  ([build-trees-and-windows-asan.md §1](build-trees-and-windows-asan.md#1-never-run-the-two-build-trees-at-the-same-time)).

Before adding an output directory, check whether a second tree of the same worktree would write the
same path. If it would and the file is an input to a later step, it belongs in the binary dir.
