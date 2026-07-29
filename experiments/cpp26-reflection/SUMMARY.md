# C++26 reflection for OloEngine — capstone summary

The top-level index for the reflection experiment. Deep-dives live in `EXHAUSTIVE_AUDIT.md`
(serializer), `OLOHEADERTOOL_REPLACEMENT.md` (all C++ outputs + Lua), `CSHARP_BINDINGS_DESIGN.md`,
`REFLECTION_TOOLCHAIN_STATUS.md`, and ADR [0009](../../docs/adr/0009-scripting-bindings-from-reflection-emitted-schema.md).

## What it proves

C++26 static reflection (P2996, GCC 16.1 `-freflection`) can **replace the entire C++ side of
OloHeaderTool and the hand-written Lua bindings, and feed C#**, from one annotation source — and do
it *more correctly, more completely, and more robustly* than the 4,540-line text parser plus
hand-written glue it replaces.

## Core results

- **Scene serializer (audited):** 105/110 components; 72 byte-key-identical; **more correct than the
  shipping engine** — the audit found 9 components where the hand-written serializer silently drops
  authored data. Handles more types than the tool (private members, `optional`, `array`,
  `unordered_set/map`, `unique_ptr`, nested structs, `Ref<Asset>`, glm mat/ivec).
- **Every C++ OloHeaderTool output:** tuple, on-add/remove, serializer, SaveGame (exact 110/110,
  exclusion set *derived*), MCP registry (655/655 top-level + more current), C++ glue (362 fields).
  Plus AssetManager resolution wired into deserialize.
- **Lua:** fully reflection-generated (61 components / 343 fields), real scripts drive live components.
- **C#:** neutral-schema architecture (ADR 0009), proven end-to-end (reflection → schema → C#) for
  fields, methods, and events. All four ADR commitments implemented.

## "As far as we can on GCC" — the 7 completion items

1. **Real in-engine integration.** The real `SceneSerializer.cpp` **compiles under GCC-16 + reflection
   (0 errors)**, and a real `SerializeEntityViaReflection(Entity)` (using the live `HasComponent`/
   `GetComponent` API + reflection serializer, over all serializable components) **codegens to a
   25.6 MB object file** with the linkable symbol emitted. Moves from "probes" to "real engine
   object code." *Remaining:* a full OloServer binary link would need all vendor libs rebuilt with
   the in-tree GCC-16 — a toolchain undertaking, not a feasibility question.
2. **Unified GenerateBindings driver** (`gen_all.cpp`): one program emits every artifact — 120
   components → 110 tuple / 110 SaveGame / 834 MCP fields / 361 glue fields / schema.
3. **Round-trip validation** (`roundtrip.cpp`): **105 PASS, 0 FAIL** — every serializable component
   serialize→deserialize→re-serialize is identical (exercises the AssetManager CPO).
4. **Build-time characterization:** engine-header baseline 22.0s (paid either way); reflection adds
   +4.3s (all lists+schema) to +11.9s (full serializer) — affordable, paid only in generating TUs.
5. **Both GCC-16 "workarounds" resolved:** (a) value-driven MCP nested recursion emits the 197 nested
   dotted paths the type-driven form couldn't (real GCC bug, clean workaround); (b) the Lua
   arg-method "limit" was a *syntax error* — `&[:m:]` forms member pointers for fields *and* functions
   (`&C::[:m:]` was wrong); Lua now binds fields + arg-methods via member pointers, driven live.
6. **Events** — the reserved third schema kind, populated end-to-end: `OLO_EVENT` → schema
   `kind:"event"` with the callback signature → C# `public event System.Action<Vector3, float> …`.
7. **Test-collapse analysis** (below).

## #7 — coverage tests that reflection retires

Reflection makes the whole *"a hand-maintained list forgot a component"* bug class **structurally
impossible** (discovery is a namespace scan). The tests that exist *only* to catch that class
collapse entirely:

| Suite | Purpose | Fate under reflection |
|---|---|---|
| `ComponentTupleCoverageTest` | every component is in the AllComponents tuple | **unnecessary** — discovery is exact |
| `ComponentSerializerCoverageTest` | existence + hand/generated disjointness | **unnecessary** — no split; by construction |
| `SaveGameComponentSerializerCoverageTest` | the save lists match | **unnecessary** — `requires`-derived |
| `ComponentHandlerCoverageTest` | on-add/remove lists match | **unnecessary** — primary template *is* the no-op |

That's **~569 LOC + 7 test cases** (`ComponentTupleCoverageTest.cpp` 291, `ComponentHandlerCoverageTest.cpp`
278) of pure completeness-guards **deleted outright**. Partially collapsing: `McpFieldRegistryTest`
(834 LOC, 39 cases — the *completeness* checks go, the field-write *behaviour* tests stay) and the two
codegen-mirror suites (`Container`/`NestedStructSerializerCodegenTest`, ~483 LOC — the "mirror the
generator's emitted shape" purpose goes; the serialization *logic* coverage can be re-expressed as
direct serializer tests). Net: reflection retires ~570 LOC of tests outright and repurposes ~1,300
more, because it converts *"guard against forgetting"* into *"impossible by construction."*

## What remains (all productionisation, not feasibility)

Gated on **MSVC** shipping P2996 (none today, no ETA — see `REFLECTION_TOOLCHAIN_STATUS.md`, tracked in
issue #688). Then: wire the generators into the real build, stand up the C#/Coral Roslyn generator
against the schema, rebuild vendor libs for the full-target link, and migrate the few genuinely
hand-written components (Material/ParticleSystem embeds — the principled boundary, like ScriptComponent's
out-of-band C# data). Every *mechanism* those depend on is proven.

## Probe index (`scratchpad/`)

`real_sweep` (serializer sweep) · `roundtrip` (#3) · `gen_all` (#2) · `savegame_gen` · `mcp_gen` +
`mcp_nested` (#5a) · `scriptglue_gen` · `schema_gen` + `methods_schema` (#6) · `gen_csharp_from_schema.py`
(schema→C#) · `csharp_gen` (direct C#) · `lua_gen` + `lua_methods` (#5b) · `asset_roundtrip` · `onset_demo`
· `container_probe` · `audit.py` (the exhaustive audit).
