# Merge safety — the engine-source annotations vs the shipping OloHeaderTool

The experiment added `OLO_SERIALIZE(Skip)` / `OLO_SERIALIZE(Key, …)` annotations to 10 shipping
component headers so the GCC-16 reflection serializer would skip runtime fields. **OloHeaderTool is a
compiler-agnostic text parser** — it reads those annotations on *every* build (MSVC included),
regardless of the empty `#define OLO_SERIALIZE(...)` macro. So before merging to `master` we had to
prove the annotations don't change what the shipping tool generates in a breaking way.

This was verified empirically by **building OloHeaderTool with g++-14 and diffing its output** against
a master baseline (the tool is deterministic text-parsing, so the g++ run reproduces the MSVC tool's
output exactly). Reproduce: `g++ -std=c++20 -O1 -o /tmp/oht tools/OloHeaderTool/main.cpp`, run it with
master's headers, then with the branch's, and diff the two output trees.

## What the annotations change in the tool's output

| Generated artifact | Effect | Status |
|---|---|---|
| AllComponents tuple | none | unchanged |
| SaveGame capture/restore | none | unchanged |
| OnComponentAdded/Removed | none | unchanged |
| C#/C++ ScriptGlue | none | unchanged |
| **Scene serializer (text + binary)** | **15 components flip hand-written → auto-generated** | **fixed (see below)** |
| MCP field registry | ~25 runtime fields dropped from MCP-editable | intended behavior (see below) |

### The serializer flip (the real hazard) — fixed

`Skip` drops a field from the trivial-classification. On 15 components that dropped the *last*
non-trivial (runtime) field, leaving the authored remainder all-trivial — which flips them from
hand-written to auto-generated. Since all 15 are **also** hand-written in `SceneSerializer.cpp`, that
is a double-emit (a duplicate YAML key on save + a double `AddComponent` on load, caught by
`ComponentSerializerCoverage.NoComponentIsBothHandWrittenAndGenerated`).

The 15: `AudioListener`, `AudioSoundGraph`, `BehaviorTree`, `BoxCollider2D`, `CircleCollider2D`,
`Foliage`, `GoapAgent`, `MorphTarget`, `Ragdoll`, `Rigidbody2D`, `StateMachine`, `Terrain`,
`VideoOverlay`, `VideoSurface`, `Water`.

**Fix:** add all 15 to `kComponentsCustomSerialize` in `tools/OloHeaderTool/main.cpp`. Each genuinely
needs its hand-written serializer (enum-keyed `BodyType`, `Ref<Asset>` handle keys, load clamps,
runtime-token omission, exact on-disk shape). With the exclusions, the tool's serializer output —
text **and** binary — is **byte-identical to the master baseline**. Verified: every serializer `.inl`
diffs clean against baseline after the fix. The serializer diff was also confirmed *purely additive*
(0 removed lines) — no already-generated component lost a field, so no scene-format change.

Because master's checked-in serializer `.inl` already excludes these 15 (they were non-trivial on
master) and the exclusions keep them excluded, the checked-in `.inl` need **no** regeneration — the
`GenerateBindings` step regenerates them consistently on the first build.

### The MCP change (intended, tested) — kept

The same field scan feeds the MCP writable-field registry, so `Skip` also drops those runtime fields
from MCP-editable (`RootMotion*`, `IsRunning`, `Playing`, `NeedsRebuild`, `IsLoaded`,
`HasVisibleTarget`, …). This is the tool's **designed** behavior, already asserted by
`McpFieldRegistryTest.SkipAnnotatedRuntimeFieldsAreNotWritable` (NavAgent's Skip'd fields), and it is
correct — per-tick runtime state should not be live-editable. Verified non-breaking against the pins:
`KeepsEveryPreviouslyHandWrittenField` checks only authored fields (none removed);
`CoversTheComponentsAnAgentDebugsWith` checks component coverage (all 15 keep their authored fields);
no test writes a removed field. This is the **one** intentional shipping-behavior delta of the merge.

## Compilation

All 10 annotated headers include `ComponentReflection.h`, where `#define OLO_SERIALIZE(...)` is empty —
so every annotation expands to nothing under MSVC/clang-cl. The annotations are all well-formed
(`OLO_SERIALIZE(Skip)` / `OLO_SERIALIZE(Key, "…")`, no nested parens). The experiment already compiled
these same headers under GCC with the non-empty bridge form (a superset), so the empty form compiles.

## Final verification checklist (run once on an MSVC or clang-cl build)

The tool-output dimension is fully proven locally. These confirm the C++ side on the real toolchain:

- [ ] `cmake --build build --target GenerateBindings` — regenerates the `.inl` (should be a no-op diff
      for the serializer, a 77-line MCP delta).
- [ ] `cmake --build build --target OloEngine-Tests` — compiles (proves the empty-macro headers build).
- [ ] `OloEngine-Tests --gtest_filter=ComponentSerializerCoverage*:ComponentTupleCoverage*:SaveGameComponentSerializerCoverage*:ComponentHandlerCoverage*:ComponentRoundTrip*:McpFieldRegistry*`
      — all green (serializer/tuple/savegame/handler unchanged; MCP change is the tested Skip behavior).
