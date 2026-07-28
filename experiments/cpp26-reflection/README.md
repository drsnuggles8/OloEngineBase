# C++26 static reflection vs. OloHeaderTool — experiment

A proof-of-concept that **C++26 static reflection (P2996) + annotations (P3394)** can
replace — and exceed — OloHeaderTool's engine-side codegen, validated **live against
the real OloEngine** (all 110 components).

> **GCC 16.1 only.** Mainline Clang 21 and MSVC have neither `-freflection` nor
> `<meta>`, so this is an experiment / future-proofing exercise, **not** a drop-in for
> the shipping MSVC + clang-cl build. It builds under the OloServer/WSL2 GCC toolchain.

## What it does

OloHeaderTool is, in effect, a hand-rolled reflection engine: ~4,540 lines that parse
`struct *Component` definitions *as text* and emit `.inl` files for the ECS
`AllComponents` tuple, scene serialize/deserialize, save-game capture/restore,
`OnComponentAdded/Removed`, etc. Nearly every sharp edge in `CLAUDE.md` (the
`*Component` name-match footgun, the statement-splitter fix, private-member tracking,
the trivial/non-trivial classifier) exists *because* it parses C++ as text.

This experiment does the same job with native reflection — **~360 lines, no generated
files, no regeneration step** — and reads field metadata (`Clamp`/`Skip`/`Key`) as real
`[[=...]]` annotations instead of comment-macros.

## Results (measured against the real engine)

| Capability | Result |
|---|---|
| Full real `Components.h` (4,519 lines) compiles under GCC 16.1 | ✅ (PCH + `-fpermissive`) |
| Component discovery + `AllComponents` tuple | ✅ **exact** — 110 == the generated tuple, both directions |
| Generated-`.inl` serialize coverage | ✅ **44 / 44** byte-identical (incl. the custom-key `EnvironmentMapComponent` via `Reflect::Key`) |
| Total components auto-serialized (tool does 44) | ✅ **84 / 110** |
| Deserialize round-trip + hostile-input behavior | ✅ full parity |
| **Beyond the tool:** private members, `std::array`, glm `mat`/`ivec` | ✅ e.g. `TransformComponent` auto-serializes (private fields + `mat4`) |
| Serializer bugs | **0** |

The 26 skipped components have genuinely non-serializable members (`Ref<non-Asset>`,
function-pointer registries, runtime physics/audio tokens) — the same set the engine
hand-writes or excludes; reflection correctly declines them rather than emitting wrong data.

## The library (3 headers)

- **`OloReflectAnnotations.h`** — field annotations `Reflect::Skip` (omit), `Reflect::Clamp{min,max}`
  (clamp on load), `Reflect::Reject{min,max}` (keep the default if out of range — reject, not clamp),
  `Reflect::Key{"name"}` (custom YAML key rename). Kept separate so components can be annotated before
  the reflection code runs.
- **`OloReflect.h`** — the core: namespace-scan component discovery, the `AllComponents` tuple via
  `substitute`, `for_each_component`, and `visit_fields<T>()` (the generic member-walk; uses
  `access_context::unchecked()` so it can read **private** members — which OloHeaderTool cannot).
- **`OloReflectYaml.h`** — the full-type serializer/deserializer built on `visit_fields`: one
  recursive `EmitValue`/`ReadValue` covering scalars, enums, glm vec/mat/ivec/quat, `UUID`/`AssetHandle`
  (as u64), `std::string`, `std::vector`, `std::array`, `std::unordered_set`/`map`, nested structs,
  and `Ref<Asset>` (as its handle, omitted if null).

## Build & run

Requires the in-tree GCC 16.1. Edit `GCC16_BUILD` / `OLO_ROOT` in `build.sh` (or export them)
for your machine.

```bash
# self-contained tests (glm + yaml-cpp only)
./build.sh priv_probe.cpp        && ./priv_probe        # private members are readable via reflection
./build.sh parity_validate.cpp   && ./parity_validate   # serialize: every type category == the generator
./build.sh ref_demo.cpp          && ./ref_demo          # Ref<Asset> -> handle
./build.sh env_demo.cpp          && ./env_demo          # Reflect::Key custom key + omit-if-null
./build.sh hook_demo.cpp         && ./hook_demo         # OnDeserialized hook + Reject: NavMeshBounds/StreamingVolume/MorphTarget become reflectable

# real-engine tests (compile the actual Components.h; heavy)
./build.sh --engine deser_validate.cpp  && ./deser_validate   # deserialize round-trip on real types
./build.sh --engine --syntax real_setcheck.cpp                # PROOF: discovered set == real 110 (static_assert)
./build.sh --engine real_discover.cpp   && ./real_discover    # prints all 110 real component names
./build.sh --engine real_serialize.cpp  && ./real_serialize   # real components -> YAML matching the .inl
./build.sh --engine real_sweep.cpp      && ./real_sweep        # full 110 sweep + coverage number
```

(The run commands need `LD_LIBRARY_PATH` set to the in-tree libstdc++; `build.sh` prints the exact line.)

### Key toolchain findings (the non-obvious bits)

- **PCH is required:** `SceneCamera.h`/`Camera.h` use `f32` without including `Base.h`, relying on the
  force-included `OloEnginePCH.h` (`/FI` in the real build). `-include OloEnginePCH.h` fixes it.
- **`-fpermissive`** accepts the MSVC/Clang-isms GCC 16 rejects (e.g. a member named the same as its type).
- **Linking needs only tiny stubs**, not the HAL — the reflection TU ODR-uses just `RefUtils::Release`
  (+ `Font::GetDefault`/`SceneCamera::ctor` when constructing components); one-line no-op stubs suffice.
- `std::meta::info` can't hold a `const char*` (annotations aren't reflectable pointers) — `Reflect::Key`
  stores a `char[64]` and reads it back via `std::define_static_string`.

## The macro bridge (production path)

In the real engine, `OLO_SERIALIZE(Skip)` / `OLO_SERIALIZE(Clamp,…)` / `OLO_SERIALIZE(Key,…)` are
empty markers (for OloHeaderTool's text parse). Under `__cpp_reflection` they'd expand to the real
annotations, so **one source feeds both tools** (see the `#define OLO_SERIALIZE(mode,...)` bridge at
the top of `real_sweep.cpp`). Every reason a component stays hand-written maps to an annotation **or a
post-deserialize hook**: custom key → `Key`, runtime field → `Skip`, clamp → `Clamp`, keep-default-if-
out-of-range → `Reject`, conditional-null → omit-null, and **cross-field invariants (Min≤Max swaps,
hysteresis, per-value clamps) → an `OnDeserialized()` method reflection calls after reading the fields**
(`hook_demo.cpp`). With these, even components the engine hand-writes — NavMeshBounds, StreamingVolume,
MorphTarget — become reflection-driven: field I/O is generic, only the invariant stays a (small) method.

## Not in scope

The C# `.cs` bindings (C++ reflection can't emit C# source) and the editor MCP field registry
(MSVC-only) — unchanged from OloHeaderTool; unrelated to this experiment.
