# Replacing OloHeaderTool with C++26 Reflection — C++-side coverage

OloHeaderTool emits **seven** artifacts. This document records the reflection replacement status of
each, proven on GCC 16.1 (`-std=c++26 -freflection`). The scene-serializer (#3) has its own deep
write-up in `EXHAUSTIVE_AUDIT.md`; this file covers the rest and the whole-tool picture.

| # | OloHeaderTool output | Reflection status | Evidence |
|---|---|---|---|
| 1 | `AllComponents` tuple | ✅ **native** | `DiscoverComponents()` → `substitute(^^tuple, …)` |
| 2 | `OnComponentAdded/Removed` no-ops | ✅ **native** | primary template *is* the no-op |
| 3 | Scene serialize/deserialize | ✅ **native, audited** | `EXHAUSTIVE_AUDIT.md` — 105/110 |
| 4 | SaveGame capture/restore lists | ✅ **exact match** | `savegame_gen.cpp` — 110/110, empty diff |
| 5 | MCP writable-field registry | ✅ **top-level exact** | `mcp_gen.cpp` — 655/655 + more current |
| 6 | C++ scripting glue | ✅ **native (bulk)** | `scriptglue_gen.cpp` — 362 fields, live get/set OK |
| 7 | **C# bindings (`.cs`)** | ❌ **foreign language** | out of reach for C++ reflection — see below |

Plus the runtime-service wiring the deserialize needs:

| — | AssetManager resolution on load | ✅ **wired via CPO** | `asset_roundtrip.cpp` — handle 9999 round-trips to the resolver |

## The architectural wins (reflection is *better*, not just equivalent)

**Exclusion sets become derived predicates.** OloHeaderTool hand-maintains several *different*
exclusion lists (`kComponentsNotInTuple`, `kComponentsNotInSaveGame`, `kComponentsNotMcpEditable`, …)
— a component silently dropped from one is a real bug the coverage tests exist to catch. Reflection
replaces them with a single raw scan (`DiscoverAllComponents()`) plus a *predicate per consumer*:
- **SaveGame**: a component is in the list **iff** `SaveGameComponentSerializer::Serialize(ar, c)` is a
  valid call — a `requires`-concept. The exclusion set is *derived*, and it reproduced the engine's
  list **byte-for-byte (110/110)**, correctly deriving the 2-member exclusion (AudioSoundGraph,
  LocalizedText) with no hand-kept list.
- **Tuple**: raw set minus `IsRuntimeOnly`.
- **MCP**: raw set minus editability, public + JSON-coercible members.

The whole class of "forgot to add/exclude a component from a list" bugs becomes **structurally
impossible** — and half the coverage tests that guard the tool become unnecessary.

**Reflection is more current than the generated files.** The checked-in MCP registry `.inl` is stale:
reflection found **144 public fields across ~10 newer components** (TimeOfDay, Cloudscape, FootIK, …)
that the generated file is missing. A generated file drifts from source; reflection cannot.

**Reflection reaches private members natively.** The MCP registry needs a special `MakeSetterField`
path for private OLO_PROPERTY fields (e.g. Transform's `RotationEuler`); reflection reads/writes them
directly via `access_context::unchecked()` — no second mechanism.

## What each prototype proved

- **`savegame_gen.cpp`** — 110/110 exact match; `requires`-derived exclusion set.
- **`mcp_gen.cpp`** — 655/655 top-level fields exact; +144 more (stale-file drift); nested dotted
  paths (`Config.ConeInnerAngle`) use the same struct recursion the serializer already does via
  values (the type-driven recursive variant hits a GCC-16-experimental instantiation bug — a
  toolchain issue, not a reflection limit).
- **`scriptglue_gen.cpp`** — 362 OLO_PROPERTY fields → 724 Get/Set registrations; the generated
  get/set **logic runs correctly on a live component** (set 123.5 → get 123.5). 81% of bindings
  (plain + enum) are native; the ~12% custom `Get=`/`Set=` expressions call component methods (a
  small bespoke set, like the serializer's `kComponentsCustomSerialize`).
- **`asset_roundtrip.cpp`** — deserialize reads `Ref<Asset>` handles and resolves them through the
  `ReflectResolveAsset<T>` customization point (mock records handle 9999; the engine defines it as
  the one-liner `return AssetManager::GetAsset<T>(AssetHandle{h});`).

## Verdict: the C++ side of OloHeaderTool is fully replaceable

All six C++ artifacts are reflection-generated, the runtime-service seam (AssetManager) is wired, and
several become *more* robust (derived exclusions, no staleness, native private access). The generated
`.inl` files and the `GenerateBindings` build target disappear for the C++ side once the toolchain
ships P2996 on MSVC/Clang.

The one irreducible remainder is **#7, the C# bindings** — `.cs` proxy source that must exist as files
for the Mono compiler. C++ reflection can *enumerate* the types to drive that codegen, but it cannot
*emit C# source*; that stays a build-time codegen step (a much smaller tool, or a Roslyn source
generator fed by exported metadata).

## Bonus: the hand-written Lua bindings collapse into reflection too

`LuaScriptGlue.cpp::RegisterAllTypes()` is 105 hand-written sol2 `new_usertype<C>` registrations —
not an OloHeaderTool output, just hand-maintained C++. Because it's C++, it's fully in reflection's
reach. `lua_gen.cpp` **compiles against real sol2 + Lua** and, driven purely by reflection over the
components' OLO_PROPERTY fields, registered **61 components with 343 fields** as sol2 properties —
then **real Lua scripts drove live components through the generated bindings**:
`tc.translation.x = 5` (glm-vec sub-field write via a reference getter), `rb.mass = 42` (scalar), and
`rb.type = 1` (enum↔int) all took effect on the C++ objects.

Type dispatch is generic: enum↔int, `AssetHandle`↔u64, `glm::vec2/3/4` via reference getter (so
sub-field assignment hits the real object), scalar/string by value. The bespoke tail is the same
shape as the serializer's: the 9 heavyweight-embed components (Material/ParticleSystem/gameplay-tree
— which the hand-written glue also doesn't expose), private OLO_PROPERTY fields needing getter/setter
methods (Transform's `rotation`), and the physics-synced setters (Rigidbody2D→Box2D) that carry
runtime side effects. Everything else is generated.

**So both scripting backends split the same way:** Lua (C++) is reflection-native like the rest;
C# (`.cs` source) is the sole piece that needs a codegen step reflection can *feed* but not *be*.

Two GCC-16-experimental notes for the Lua path: sol2 is compile-heavy (~2 min for 61 usertypes under
the reflection frontend — a build-time cost, fine for a one-time registration), and a
pointer-to-member cannot yet be formed from a reflection via `&C::[:m:]` (used a reference-getter
lambda `[](C& c) -> MT& { return c.[:m:]; }` instead, which sol2 accepts).
