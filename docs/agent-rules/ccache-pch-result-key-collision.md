# A cached PCH must never be shared across checkout roots

When ccache's `base_dir` makes two checkouts at different absolute paths hash alike, keep
the **PCH-producing** compile out of that sharing: run it with `base_dir` cleared.
`scripts/ccache-pch-launcher.sh` does this, and `cmake/CompilerCache.cmake` wires it up for
ccache on POSIX hosts. Objects can be shared across roots, because `-ffile-prefix-map` makes
their bytes root-independent. A `.pch` cannot be shared: clang records the absolute path of
every header it read.

Issue #1460. It showed up as one red job whose next run, on the same diff, passed.

## What it looked like

The GL-only job on `olo-ci-2` (run 36045750117) failed compiling `OloEnginePCH.cpp`:

```
In file included from ../../../OloEngine/src/OloEnginePCH.h:29:
../../../OloEngine/src/OloEngine/Core/Base.h:120:16: error: redefinition of 'ArraySize'
/home/gh-runner-olo/actions-runner-ci-1/_work/.../Core/Base.h:120:16: note: previous definition is here
```

The previous definition is in the **other** runner slot's checkout. The job's
`cmake_pch.hxx.pch` had "built" in 0.6 s, which is a cache hit. The consumer then included
`Base.h` through its own root, and `#pragma once` counts a header at a different path as a
different file.

## Why the keys collide (the non-obvious part)

A slot never matches the other slot's direct-mode entry, so it looks as if nothing could
be shared. The collision is one level further down:

1. **ccache serves a PCH from direct mode only.** Its log says *"Not considering cached
   precompiled header in preprocessor mode"*.
2. **The direct-mode manifest holds one entry per slot.** Both slots use the same manifest
   key, but an entry records the hash of every include file, including `cmake_pch.hxx`.
   CMake writes that file's `#include` line as an absolute path, so each slot matches only
   its own entry.
3. **The result key is shared.** It is computed from the preprocessed output, and
   `base_dir` rewrites the include paths in that output to relative ones. Both slots
   computed the same result key, `20106m1h…`, and each stored its own `.pch` under it.

So slot 1 builds and stores its `.pch`. Slot 2 builds and overwrites the same result.
Slot 1's next build follows **its own** manifest entry to that result and gets slot 2's
`.pch`. The build fails only when a consumer also misses. A consumer that hits never loads
the `.pch`, which is why the failure was rare and went away on re-run.

## Reproduction (measured)

Linux, clang 21 + ccache 4.11.3 (the box's version), a 4-TU CMake project with a real
`target_precompile_headers()` PCH. Two roots named `actions-runner-ci-{1,2}/_work/Olo`
share one cache directory with the action's env. The sequence is slot 1, slot 2, then
slot 1 again from a fresh build tree with one consumer edited:

| arm | step 3 | `.pch` served in step 3 |
|---|---|---|
| plain ccache, 1→2→1 | **fail**, previous definition in slot 2 | direct hit, all 9 paths name slot 2 |
| plain ccache, 2→1→2 | **fail**, mirror image | all name slot 1 |
| launcher, both orders | pass | direct hit on its own `.pch` |
| no `base_dir` (the AMD conformance job) | n/a | PCH misses across slots: keys carry the root |

`scripts/check-ccache-pch-two-root.sh` is this reproduction. It runs in `vulkan-off.yml` on the
box, with plain ccache as a control that must fail and the launcher as the arm that must pass.

## What the fix costs: nothing that was really being shared

Sharing measured with and without the launcher was identical. The non-PCH object hits across
slots in both. **PCH consumers miss across slots in both**, because a consumer's key
includes the `.pch` file's hash, and that differs per slot. The earlier claim in
`setup-linux-build` that objects were byte-identical across slots "PCH included" was true
of the object bytes but not of the keys. Only non-PCH objects were ever shared across slots.

Turning the PCH off for self-hosted jobs would let the consumers share too, at the price of
a slower compile on every miss. That trade-off has not been priced. Do not change it here
without measuring it.

## Rules

- **An output whose bytes depend on the absolute root must not be cached under `base_dir`.**
  Today that is the PCH. Before widening `base_dir` sharing to a new kind of output, check
  whether that output embeds paths the way a `.pch` does.
- **Check a shared-cache claim with a consumer miss after a producer hit.** The earlier
  measurement compared object bytes and passed. The failure needs a PCH hit, then a
  consumer that misses and re-includes a PCH header, and it needs the slots in both orders.
- **Windows ccache does not reproduce this.** It did not relativise the preprocessed include
  paths in a clang GNU-driver test (ccache 4.13.6), so the two keys never collided there.
  The launcher is POSIX-only for that reason, and a Windows reproduction is not evidence.
- Local `dev-cached` is clang-cl, so it is unaffected: the MSVC frontend's PCH is already
  disabled under caching (`CompilerCache.cmake`, the `_olo_pch_uncacheable` block).

Related: [self-hosted-ccache-slot-multiplier.md](self-hosted-ccache-slot-multiplier.md) (why
the slots share a cache), [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md)
(why the PCH is cacheable at all).
