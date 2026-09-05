# Component scene-serializer codegen — the eligibility rule, the annotations, and the one blind spot

OloHeaderTool generates the per-component serialize/deserialize blocks in
`Scene/SceneSerializer.cpp`. A plain all-trivial component round-trips through scene YAML
with **zero** hand-written code. Read this before adding a component field, widening the
classifier, or reaching for a `kComponentsCustomSerialize` exclusion.

Started as issue #380's fourth slice; every widening since is a slice of issue #451.

---

## 1. What is generated, and where it lands

The generator emits two files into `OloEngine/src/OloEngine/Scene/Generated/`:

| File | `#include`d at |
|---|---|
| `SceneSerializeComponents.Generated.inl` | the end of `SerializeEntity` |
| `SceneDeserializeComponents.Generated.inl` | the end of `DeserializeEntityComponents` |

Both are driven by **`CollectComponentFields`** — a full data-member scan, **not** the
`OLO_PROPERTY` scan. The serializer persists *every* field, not just script-exposed ones
(`DirectionalLightComponent` serializes 9 fields but annotates only 3).

The generated deserialize validates floats via `TryReadFiniteF32` (scalars) and the glm
`Decode` helpers (vectors). A missing key keeps the constructor default.

`kComponentsCustomSerialize` (`tools/OloHeaderTool/main.cpp`) is a **fourth** exclusion set,
unrelated to the tuple / save-game / on-add / on-remove sets.

---

## 2. The eligibility rule

A component is emitted iff **every non-skipped data member** is serializer-trivial **and public**,
and the component is not in `kComponentsCustomSerialize`. Members annotated `OLO_SERIALIZE(Skip)`
(§3) are excluded from that judgement entirely — they neither have to be trivial nor public, which
is what lets an otherwise-all-trivial component keep one runtime field and still be generated. The
classifier's own type enum is `PropType`; `SceneSerType(...)` is the function that maps a written
member type onto it. Enum members are recognised via `CollectEnumTypes`, nested structs via
`CollectStructBodies`.

### Trivial member types

| Type | Round-trip | Added by |
|---|---|---|
| `bool`, `int`, `u32`, `u64`, `f32`, `f64` | native yaml-cpp | #380 |
| `std::string` | native | #380 |
| `glm::vec2/3/4` | `Encode`/`Decode` in `Core/YAMLConverters.h` | #380 |
| `AssetHandle`, `UUID` (a `u64` wrapper) | `static_cast<u64>` on write; `.as<u64>` + the implicit `UUID(u64)` ctor on read | #451 |
| `enum` / `enum class` | `static_cast<int>` on write; `.as<int>` on read, cast back via `static_cast<decltype(comp.member)>` | #451 enum slice |
| `glm::quat`, `mat3`, `mat4`, `ivec2/3/4` | `Encode`/`Decode` helpers — the slice added `quat`/`ivec2`/`ivec4` converters plus `mat3`/`mat4`/`quat`/`ivec2`/`ivec4` `Emitter<<` overloads to `Core/YAMLConverters.h` | #451 glm slice |
| `u8`, `u16`, `i8`, `i16` | widened to `u32`/`i32` on emit; read via `.as<decltype(member)>` | #451 glm slice |
| an all-trivial nested `struct` | nested `BeginMap`…`EndMap` sub-map keyed by the member name | #451 nested-struct slice |
| `std::vector<one of the above>` | YAML sequence (`BeginSeq`); a `std::vector<struct>` is a sequence of sub-maps | #451 glm + nested-struct slices |
| `std::unordered_set<sortable trivial scalar>` | YAML sequence via a sorted temporary | #451 unordered slice |
| `std::unordered_map<std::string, V>` | genuine YAML mapping via sorted keys | #451 unordered slice |
| `Ref<T>` where `T` is `Asset`-derived | asset handle | #566 |

### Type-specific rules that are not optional

- **Small ints must be widened on emit.** yaml-cpp's `convert<unsigned char>::encode` writes a
  **raw char**, not a number. The `u32`/`i32` cast is mandatory. The read side uses
  `.as<decltype(member)>`, whose char-type decode special-case parses and range-checks.
- **`enum` recognition is leaf-name-based.** `CollectEnumTypes` scans every `enum`/`enum class`
  definition under `OloEngine/src` and matches the member type's leaf name — which is why a
  **nested** enum like `AnimationStateComponent::State` needs no qualified spelling at the
  SceneSerializer level.
- **`std::vector<E>` element reads go through `::YAML::convert<value_type>::decode`** and skip a
  malformed element. `f32` elements are finiteness-checked; `enum` elements are int-bridged.
- **Unordered containers sort before writing.** Unlike `std::vector`'s already-deterministic
  declaration order, an unordered container's iteration order is implementation-defined and can
  vary run-to-run. Without the sort, two identical scenes serialize to different-but-equal YAML
  and break `ComponentRoundTrip.SerializeLoadSerializeProducesIdenticalYAML`. A set emits through a
  temporary `std::vector<E>`; a map through a sorted `std::vector<std::string>` of keys.
- **A `std::unordered_set` element must be *sortable*.** Eligible: `bool`, `int`, `u32`, `u64`,
  small ints, `AssetHandle`, `std::string`, `enum`. **Not** `float`, a glm vector/matrix/quat, or a
  struct — none define a meaningful `operator<`, and sorting a sequence containing `NaN` is
  undefined behavior. An `AssetHandle` element sorts via an explicit `static_cast<u64>` comparator
  (`UUID` has no `operator<` of its own, only an implicit conversion to `u64`) — the same reasoning
  as the scalar `AssetHandle` write cast.
- **A `std::unordered_map` value** may be any element type `std::vector` accepts, excluding a
  nested struct. The map's on-disk shape matches the hand-written `MorphTargetComponent::Weights`
  idiom this slice mirrors.

### Nested structs

The parser builds a leaf-name→body registry of *every* struct under `OloEngine/src`
(`CollectStructBodies`). For a member whose type is neither a built-in, an enum, nor a recognised
container, it recursively classifies that struct via `ClassifyStruct` — mutually recursive with
`ParseComponentFields`, with a `visited` cycle-guard. The struct is eligible iff **every** member is
itself serializer-trivial *and public*, **and** the record is **non-empty** and **acyclic**: an
empty record and a re-entrant type are both rejected (`ClassifyStruct` returns `std::nullopt`).
Within those constraints the emit is a recursive pair
(`EmitSerializeFields`/`EmitDeserializeFields`), so nesting composes to any depth — e.g.
`LODGroup` → `Levels[]` → each `LODLevel`.

> **The parser-robustness fix this required.** The statement-splitter terminates a statement at the
> brace-depth-0 `}`, not only at `;`, because an inline method body carries no trailing `;` and
> would otherwise merge forward and swallow a following member or an intervening `private:` label.
> Without it, `ColliderMaterial` (friction/restitution getters, then `private:` floats) is mis-seen
> as all-public-trivial and generates **private-member-access code that won't compile**. With it,
> the private members are correctly seen, the struct is correctly rejected, and the six 3D colliders
> stay hand-written.

### What is still not handled

`std::array`, a non-`std::string`-keyed `std::unordered_map`, a `std::unordered_set` of a
non-sortable element, a `std::vector` of a *non-trivial* struct, and `Ref<T>` of a
non-`Asset`-derived type. The parser classifies these non-trivial and **skips the component
automatically** — no exclusion entry needed.

### A non-public member fails safe

The parser tracks `private:`/`protected:`. A non-public data member can't be reached as
`comp.member`, so the component stays hand-written automatically — no exclusion entry, no compile
error, no silent drop. This is why `TransformComponent`, with its private `Rotation`/`RotationEuler`
and euler-sync, needs **no** `kComponentsCustomSerialize` entry.

---

## 3. Per-field annotations — prefer these to a whole-component exclusion

The marker macro lives in `Scene/ComponentReflection.h`, alongside `OLO_PROPERTY`.

### `OLO_SERIALIZE(Skip)`

For a runtime-only field the serializer must omit. The field parser drops it from the generated
(de)serialize **and** does not let it mark the component non-trivial, so an otherwise-all-trivial
component with one runtime field is still fully generated (the field reloads at its constructor
default).

It composes with `OLO_PROPERTY` on the same field — a runtime field still exposed to scripts, e.g.
`UIButtonComponent::m_State`. Order between the two markers doesn't matter and either placement
works, own-line or glued onto the field: the `OLO_PROPERTY` scanner peels a leading
`OLO_SERIALIZE(...)` off the anchor line via the shared `PeelSerializeMarker` helper rather than
mistaking it for the field.

### `OLO_SERIALIZE(Clamp, Min=…, Max=…)`

Saturates an out-of-range value to the nearest bound — the `SanitizeFloat`/`std::clamp` idiom.

Supported on scalar `Float`/`Int`/`UInt`/`SmallInt`/`SmallUInt`/`Enum` **and** `glm::vec3`
(`kClampEligible` in `ParseComponentFields`). An enum clamps its underlying `int` before the cast
back, matching `static_cast<EnumType>(std::clamp(intValue, lo, hi))`. A `glm::vec3` appends a
component-wise `glm::clamp`/`glm::max`/`glm::min` step via `ApplyVec3Clamp` (the vec3 sibling of
`ApplyClamp`) *after* the plain finite-validated `.as<glm::vec3>(lhs)` read — mirroring the
hand-written `SanitizeVec3Clamped` idiom, except the finite-fallback stays whole-vector (from
`DecodeVec3`); the annotation only adds the range step on top.

Give at least one of `Min`/`Max` — both is `std::clamp`, one only is a one-sided
`std::max`/`std::min`. For a non-enum field both bounds are cast to the field's own type at emit
time, so `Min = 0` is fine on a float field. For an **enum** the comparison happens in the
underlying integer type — bounds and value are compared as `int`, and only the clamped result is
cast back to the enum — so the bounds are never cast to the enum type itself. Requesting `Clamp` on any **other** type marks the whole component non-trivial rather
than silently dropping the annotation.

### `OLO_SERIALIZE(Reject, Min=…, Max=…)`

Same bounds, but an out-of-range value — and, for a float, a non-finite one — leaves the field at
its **constructor default** instead of saturating.

**Reach for `Reject` whenever saturating would turn a corrupt value into a *different valid* one.**
The rule of thumb: `Clamp` for a continuous quantity where "as close as we can get" is meaningful
(a density, a radius); `Reject` for a discriminated one where it is not (an enum, a mode index).

> **The motivating case.** `VehicleComponent::m_DriveMode` under `Clamp(0, 2)` saturated a corrupt
> `7` to `2` = `AllWheelDrive` — a perfectly legal mode that silently drove the wrong axles and
> disagreed with **both** `SaveGameComponentSerializer` and `JoltScene::CreateVehicle`, which map
> anything that isn't Front/AllWheelDrive back to `RearWheelDrive`. No test caught the divergence,
> because the loaded value was still a valid enumerator and the car still drove. Pinned now by
> `ComponentRoundTrip.VehicleDriveModeRejectsOutOfRangeToTheDefault`.

Scalars only — **not** `glm::vec3`, since rejecting one bad component would leave a half-updated
vector. Mutually exclusive with `Clamp`; requesting both, or an unsupported type, marks the whole
component non-trivial (fail-safe).

`Reject` applies on **both** generated *read* paths — the YAML reader and the **binary scene
reader** (`.scenebin`). It is a read-side guard only; the writers are untouched.
`RejectRangeCondition` emits the in-range test; the YAML emitters guard the assignment with it,
while `EmitBinaryReject` — called from **`EmitBinaryReadFields`**, the mirror of
`EmitBinaryWriteFields` — reads into a scoped temp first, because the binary path's
read-then-fix-up would already have clobbered the default. Review both together: the two emitters
must stay field-for-field aligned or the binary format desynchronises from what the reader expects.

At the **MCP** boundary the field is still *clamped* to the same bounds rather than refused:
`MakeField`'s registry can only range a write, and a bounded write beats an unvalidated one.

---

## 4. When you must still hand-write a block

Two cases:

1. **A still-unhandled non-trivial field** (§2). The parser skips the component automatically — no
   exclusion entry needed.
2. **An all-trivial component that needs deserialize logic the plain round-trip would drop**: a
   cross-field invariant, a `Sanitize*` call, a non-`m_`-stripped key, or entity-identity handling.
   Keep the hand-written block **and** add the component to `kComponentsCustomSerialize`.

A plain range clamp and a plain runtime-field omission are **no longer** reasons to reach for case
2 — use `Clamp`/`Reject`/`Skip` instead.

Components deliberately kept hand-written, and why:

| Component | Why it can't be generated |
|---|---|
| `LODGroupComponent` | hand-written flattens the `LODGroup` sub-struct to top-level `Bias`/`Levels` keys and omits runtime `m_GeneratedLODHandles` — a nested `LODGroup:` sub-map would change the on-disk format |
| `NavMeshBoundsComponent` | `SanitizeVec3`s Min/Max, orders Min ≤ Max, drops non-finite links, clamps Radius |
| `LightProbeVolumeComponent` | omits runtime `m_Dirty` / `m_ShowDebugProbes`, plus Resolution/Spacing/Intensity clamps and a cross-field invariant a single-field annotation can't express |
| `StreamingVolumeComponent` | omits runtime `IsLoaded`, plus radius clamps with a `UnloadRadius > LoadRadius` hysteresis invariant |
| `PhysicsJoint3DComponent` | omits runtime `m_RuntimeConstraintToken`; otherwise far too large and varied — dozens of clamps, enum range-guards without fallback, a local `sanitizeVec3` lambda, a `vector<vec3>` with per-element drop. A dedicated future slice, not an annotation pass |
| `SphereAreaLightComponent`, `ProceduralSkyComponent` | reject-not-clamp semantics entangled with other per-field `Sanitize*` work. `SphereAreaLightComponent` is the only component still needing a *dedicated* vec3 mechanism |
| `MorphTargetComponent` | a `Ref<MorphTargetSet>` field (`MorphTargetSet` is `RefCounted` but not `Asset`-derived, so `Ref<T>` classification doesn't reach it) plus a per-value `[0, 1]` clamp on load via `SetWeight()`. Its `std::unordered_map<std::string, f32> Weights` would classify trivial on its own |
| `Rigidbody3DComponent` | its enum is keyed `BodyType`, not the `m_`-stripped `Type`, plus a runtime `m_RuntimeBodyToken` and many fields the hand-written serializer omits |
| the three runtime `*StateComponent`s | `DialogueStateComponent`, `NoiseAnimationStateComponent`, `SpringBoneStateComponent` — per-tick state that must **not** be serialized |
| `IDComponent` | entity identity; see the blind spot in §5 |

---

## 5. The guards — and the one blind spot

`ComponentSerializerCoverageTest` runs two checks:

- **Existence** — every component is handled by `SceneSerializer.cpp` ∪ the two generated `.inl`,
  read as one corpus.
- **Disjointness** — `NoComponentIsBothHandWrittenAndGenerated`. Forgetting a
  `kComponentsCustomSerialize` entry means the component is emitted **and** hand-written: a
  double-emit, i.e. a duplicate YAML key on write and `AddComponent<T>` twice on read. Loud, never
  silent.

`ContainerSerializerCodegenTest.cpp` and `NestedStructSerializerCodegenTest.cpp` mirror the
generator's exact emitted shape for cases no live component exercises yet — today that is the
`std::unordered_map` path and an `AssetHandle`/`enum`-element `std::unordered_set` — so those paths
still get compile and runtime coverage.

> ### The blind spot: a component persisted some *other* way than a sub-map
>
> `IDComponent`'s `UUID` is the top-level `Entity: <uuid>` line, re-applied via
> `CreateEntityWithUUID`. That makes it invisible to **both** checks: the existence check skips it
> (it's in the test's `kRuntimeOnly` set) and the disjointness check skips it (it has no
> hand-written `out << YAML::Key` block).
>
> If a classifier-widening makes such a component all-trivial, it gets auto-generated into a bogus
> sub-map plus a double-`AddComponent` on load with **no test failure**. It must be added to
> `kComponentsCustomSerialize` by hand — as `IDComponent` now is.

**So: when you widen the classifier, rebuild `GenerateBindings` and diff the generated `.inl`.**
Every slice below flipped components nobody predicted. That diff is the only thing that catches a
newly-flipped component which omits a field, clamps on load, or is runtime-only.

---

## 6. Slice history — which components each widening flipped

Kept because it is the evidence for the diff-the-`.inl` rule: every slice flipped something.

| Slice | Newly needed an exclusion | Cleanly migrated to generated |
|---|---|---|
| **#451 enum** | `Rigidbody3DComponent`, `StreamingVolumeComponent`, `FogVolumeComponent`, `UIButtonComponent`, `UISliderComponent` | — |
| **#451 glm / small-int / vector** | `LightProbeVolumeComponent`, `NavAgentComponent`, `PhysicsJoint3DComponent` | `InstancePortalComponent` (u8), `QuestGiverComponent` (`vector<string>`), `RelationshipComponent` (`UUID` + `vector<UUID>`) |
| **#451 nested-struct** | `LODGroupComponent`, `NavMeshBoundsComponent`, `DialogueStateComponent`, `NoiseAnimationStateComponent`, `SpringBoneStateComponent` | — |
| **#451 Skip** | — | `UIButtonComponent` (`m_State`), `UISliderComponent` (`m_IsDragging`) |
| **#451 Clamp** | — | `FogVolumeComponent` (4 float clamps + an enum clamp on `m_Shape`), `SnowDeformerComponent` (4 float clamps), `SpringBoneComponent` (`ChainLength` UInt `Min`-only + 3 float clamps; `Gravity` needed no annotation — its `SanitizeVec3` fallback-only behavior already matches the generated vec3 decode default), `NavAgentComponent` (5 float clamps + 6 `Skip` tags on every runtime nav-state field — without `Skip` all of them are individually trivial and would silently start round-tripping runtime pathfinder state into scene YAML) |
| **#451 vec3-Clamp** | — | `BuoyancyComponent` (`m_ProbeExtents` + 5 scalar floats), `NoiseAnimationComponent` (`RotationAmplitude`/`TranslationAmplitude` + `ChainLength`/`Octaves`/`Frequency`/`Lacunarity`/`Gain`/`Weight`) — both had been excluded *solely* for this reason |
| **#451 Reject** | — | `VehicleComponent::m_DriveMode` moved `Clamp(0, 2)` → `Reject(0, 2)` |
| **#451 unordered_map/set** | — | `PrefabComponent` (three `std::unordered_set<std::string>` override-tracking fields) — hand-written solely because `unordered_set` wasn't recognised yet, so it needed no exclusion before or after |

---

## Related

- The other four OloHeaderTool component touch-points (the `AllComponents` tuple, the SaveGame
  capture/restore lists, the `OnComponentAdded`/`OnComponentRemoved` no-ops, the MCP field registry)
  are summarised in `CLAUDE.md` → *Definition of done* → *Cross-binding check*.
- [binary-format-versioning.md](binary-format-versioning.md) — versioning the binary archive the
  `Reject` path also writes through.
- [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) §1b — how the codegen is wired
  into the build graph, and how to force a regeneration.

## 7. Every generated touch-point, its exclusion set, and what stays hand-maintained

OloHeaderTool ([tools/OloHeaderTool/](../../tools/OloHeaderTool/)) scans `OloEngine/src/` for every
`struct *Component` definition and emits seven artefacts. Six have an exclusion set in
`tools/OloHeaderTool/main.cpp`; the sets are deliberately different and each is mirrored by a
coverage test that must be kept in sync. Two of the sets are deliberately SHARED between the
MCP and visual-script field registries — `kComponentsNotFieldEditable` and
`kHandWrittenFieldClamps` are spelled without an `Mcp` prefix for that reason. Split one only
when the two consumers genuinely need to differ, and say why in the same commit.

| Artefact | Lands in | Exclusion set | Coverage test |
|---|---|---|---|
| `AllComponents` tuple (scene copy, prefab, `HasComponent<T>()`) | `Scene/Generated/AllComponents.Generated.inl`, included at the bottom of `Scene/Components.h` | `kComponentsNotInTuple`: runtime-only `IDComponent`/`TagComponent`, per-tick `*StateComponent` / `UIResolvedRectComponent` | `ComponentTupleCoverageTest` (`kNotInTuple`) |
| `OnComponentAdded<T>` / `OnComponentRemoved<T>` no-ops | `Scene/Generated/OnComponent{Added,Removed}.Generated.inl`, included by `Scene.cpp` | `kComponentsCustomOnAdd` / `kComponentsCustomOnRemove`: components with a real hand-written body. The two sets differ (`CameraComponent` does work on add only; `Rigidbody2DComponent` on remove only). `Skeleton` is not a `*Component` and stays hand-written. | `ComponentHandlerCoverageTest` |
| Scene YAML serialize/deserialize blocks | `Scene/Generated/Scene{Serialize,Deserialize}Components.Generated.inl`, included by `SceneSerializer.cpp` | `kComponentsCustomSerialize`: hand-written blocks (§4). A non-trivial or non-public member skips the component automatically with no exclusion. | `ComponentSerializerCoverageTest` |
| Save-game capture/restore lists | `SaveGame/Generated/SaveGameComponent{Capture,Restore}.Generated.inl`, included by `SaveGameSerializer.cpp` | `kComponentsNotInSaveGame`: everything without a `Serialize` overload. Keeps `IDComponent`/`TagComponent`, drops the per-tick components plus `AudioSoundGraphComponent` / `LocalizedTextComponent`. | `SaveGameComponentSerializerCoverageTest` |
| Editor MCP writable-field registry (issue #607) | `OloEditor/src/MCP/Generated/McpFieldRegistry.Generated.inl`, included by `McpGenericFieldWrite.h` | `kComponentsNotFieldEditable`: the `*StateComponent` family, `AnimationStateComponent`, `UIResolvedRectComponent`, `WorldTransformComponent`, `IDComponent`. One entry per public JSON-coercible member; `OLO_SERIALIZE(Clamp)` and the `kHandWrittenFieldClamps` table make writes clamp like a scene load. | `McpFieldRegistryTest` |
| Engine visual-script field registry (issue #793) | `OloEngine/src/OloEngine/Scripting/VisualScript/Generated/ComponentFieldRegistry.Generated.inl`, included by `ComponentFieldRegistry.cpp` | the SAME `kComponentsNotFieldEditable` and `kHandWrittenFieldClamps` as the MCP row — one scan, two consumers. One entry per public member with a `PinType` shape, so a `u64` / `ivec` / `quat` / `mat` / `Ref<T>` / container field is dropped rather than approximated. Lives under `OloEngine/src` because a graph runs in `OloRuntime` and `OloServer`, which do not link the editor. | `ComponentFieldRegistryTest` |
| C++ / C# scripting glue | `Scripting/C#/Generated/`, `OloEngine-ScriptCore/src/OloEngine/` | none; driven by `OLO_PROPERTY` annotations, not the struct scan | |

Failure modes are loud on purpose. A component in a custom-handler set without a body is a link
error; a body without the set entry is a duplicate definition; a serializer double-emit fails the
coverage test. The one silent case is a component persisted some way other than a sub-map
(`IDComponent`), which is why a classifier widening must be followed by a rebuild of
`GenerateBindings` and a diff of the generated `.inl`.

**Still hand-maintained, and not guarded by any test:**

- The save-game `Serialize` overload in `SaveGame/SaveGameComponentSerializer.{h,cpp}` and its
  `RegisterAll` registration. Without them the component is dropped from every save-game while
  round-tripping through scene YAML perfectly.
- `Scripting/Lua/LuaScriptGlue.cpp::RegisterAllTypes()`. Many components are legitimately not
  Lua-exposed, so only per-component functional round-trips exist (`tests/Lua/LuaBindingTest.cpp`).
- The editor's two per-component lists in `OloEditor/src/Panels/SceneHierarchyPanel.cpp`:
  `DrawComponent<T>` for the inspector and `DisplayAddComponentEntry<T>` for the Add Component menu
  (`TransformComponent` is inspectable but not addable). A missing entry is invisible in the editor
  and nothing else; it has gone unnoticed for several PRs.
- A component that hand-writes its copy constructor, copy-assignment and `operator==` because it
  holds `Ref<T>` runtime state (`TerrainComponent` and the others in that shape). Those three are
  per-field lists, so a new **field** trips them, not a new component. Missing from the copy path:
  `Scene::Copy` runs on every Play entry and the authored value reverts to its default. Missing from
  `operator==`: the inspector records no change and undo cannot revert it. Caught in review on #715
  after eleven fields had been missing across two shipped PRs. Guard it the way
  `ComponentRoundTripTest`'s `TerrainVirtualTextureFieldsSurviveSceneCopy` and
  `…AreVisibleToUndoEquality` do: a `Scene::Copy` round-trip plus one mutation per field.

**Two traps around the scan itself.**

- Any struct whose name ends in `Component` is swept in, ECS or not. A helper record named that way
  fails the build in unrelated TUs with `C2065: undeclared identifier` and a `C3544` pack error.
  Give registry records and DTOs another suffix, then rebuild `GenerateBindings`.
- The coverage tests parse generated files as text (for example `CollectTupleMembers` reads the
  `AllComponents = ComponentGroup<…>` marker). When a touch-point moves from hand-written code into
  a generated file, repoint the test's parser or it fails on an empty parse.

Build-graph wiring, the depfile gate and how to force a regeneration: §1b of
[build-trees-and-windows-asan.md](build-trees-and-windows-asan.md).
