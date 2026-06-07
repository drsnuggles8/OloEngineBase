# Procedural Terrain Generation

Implements [issue #113](https://github.com/drsnuggles8/OloEngineBase/issues/113)
(terrain side). OloEngine already had a mature terrain *rendering* stack —
chunked + quadtree LOD + tessellation, streaming, splat-mapped PBR material
layers, voxel overrides and foliage instancing — plus a single-fBm
`TerrainData::GenerateProcedural`. What it lacked was a real **generation**
system: the procedural path produced a grey, untextured noise heightmap that
you then had to hand-paint. This feature closes that gap.

Two things are now generated, not authored:

1. **A richer height field** — fBm plus ridged multifractal, domain warp,
   terracing and height redistribution.
2. **The material splatmap** — material layers are auto-assigned by height and
   slope rules, so a generated terrain comes out textured (sand → grass → rock →
   snow) and ready for the existing slope-aware foliage scatter.

Level/dungeon generation (rooms, prefab stamping) is explicitly **out of
scope** here — this is terrain.

---

## Module

[`OloEngine/Terrain/TerrainGenerator.{h,cpp}`](../OloEngine/src/OloEngine/Terrain/TerrainGenerator.h)
— a stateless utility. The math helpers are pure CPU (unit-tested, no GL); the
two entry points additionally touch the GPU.

### Height field

`TerrainGenerator::GenerateHeightField(out, HeightParams)` (pure) /
`GenerateHeightmap(TerrainData&, HeightParams)` (uploads). `HeightParams`
carries the base fBm knobs (mirroring the existing `TerrainComponent`
procedural fields) plus a `TerrainHeightShaping`:

| Knob | Effect |
|---|---|
| `RidgeBlend` | 0 = rolling fBm hills, 1 = sharp ridged-multifractal mountains (per-sample blend) |
| `WarpStrength` / `WarpFrequency` | Domain warp — displaces sample coords by a second noise field so ridges meander instead of following the lattice |
| `TerraceSteps` / `TerraceSharpness` | Quantize height into flat plateaus (mesa / rice-paddy look); 0 = off |
| `HeightExponent` | Redistribute the normalized field; >1 flattens lowlands and sharpens peaks (islands / deep valleys) |

Every shaping field defaults to an identity transform, so a default
`TerrainHeightShaping{}` reproduces the legacy plain-fBm terrain — existing
scenes are unchanged. Generation is fully deterministic in `Seed`, which is the
precondition for the golden-render evidence test and for reproducible scenes.

### Auto-material (splatmap)

`TerrainGenerator::GenerateSplatmap(material, data, rules, splatRes, worldX,
worldZ, heightScale)` evaluates a list of `TerrainLayerRule`s per splatmap texel
and writes the two RGBA8 splatmaps (layers 0–3 → splatmap 0, 4–7 → splatmap 1).

Each rule maps a **height band × slope band** onto one material layer:

```cpp
struct TerrainLayerRule {
    u32 LayerIndex;
    f32 MinHeight, MaxHeight, HeightBlend;   // normalized [0,1] height + soft edge
    f32 MinSlopeDeg, MaxSlopeDeg, SlopeBlend; // surface slope in degrees + soft edge
    f32 Strength;                             // weight multiplier
};
```

Height comes from `TerrainData::GetHeightAt`, slope from
`TerrainData::GetNormalAt` (`acos(normal.y)`). Overlapping rules sum per layer
then normalize, so transitions blend; a texel that no rule claims falls back to
layer 0 (never an all-zero/black splat weight).

`MakeDefaultLayers()` + `MakeDefaultRules()` provide a ready sand/grass/rock/snow
biome (solid colours — no texture files needed) so "enable procedural + auto
material" yields a textured planet out of the box.

---

## Wiring

- **Component** — [`TerrainComponent`](../OloEngine/src/OloEngine/Scene/Components.h)
  gains `m_HeightShaping`, `m_AutoMaterial`, `m_LayerRules`,
  `m_SplatmapGenResolution` (all serialized) and a runtime
  `m_AutoSplatNeedsRebuild` flag.
- **Runtime** — [`Scene::ProcessScene3DSharedLogic`](../OloEngine/src/OloEngine/Scene/Scene.cpp)
  routes the procedural path through `TerrainGenerator::GenerateHeightmap`, and
  after the material's texture arrays are built, regenerates the splatmap via
  `GenerateSplatmap` whenever the height field or the rules change. Runs on both
  the runtime and editor render paths.
- **Serialization** — `SceneSerializer` (YAML) and `SaveGameComponentSerializer`
  (binary) round-trip the new fields. The save-game format version was bumped
  **2 → 3** (`kSaveGameFormatVersion`); older saves are rejected by the header
  check rather than misread.
- **Editor** — the Terrain inspector
  ([`SceneHierarchyPanel`](../OloEditor/src/Panels/SceneHierarchyPanel.cpp)) adds
  the shaping sliders under *Procedural Generation* and an *Auto-Material*
  section with an "Apply Default Biome Preset" button, an editable rule list and
  "Generate Splatmap Now". Height-shaping edits apply on **Regenerate** (the
  same deferred-apply model the existing procedural params use).

---

## Scripting (C# / Lua)

Implements [issue #293](https://github.com/drsnuggles8/OloEngineBase/issues/293).
The scalar generation params are exposed to **both** scripting layers so gameplay
code can drive procedural worlds at runtime — pick a seed per run, regenerate on
a level transition, scale `HeightScale`/shaping from game state — instead of
terrain being purely editor-authored.

Exposed (read/write) on `TerrainComponent`:

| Script name | Backing field | Type |
|---|---|---|
| `ProceduralEnabled` | `m_ProceduralEnabled` | bool |
| `Seed` | `m_ProceduralSeed` | int |
| `Resolution` | `m_ProceduralResolution` | uint |
| `Octaves` | `m_ProceduralOctaves` | uint |
| `Frequency` / `Lacunarity` / `Persistence` | `m_Procedural*` | float |
| `WorldSizeX` / `WorldSizeZ` / `HeightScale` | `m_WorldSize*` / `m_HeightScale` | float |
| `RidgeBlend` / `WarpStrength` / `WarpFrequency` | `m_HeightShaping.*` | float |
| `TerraceSteps` | `m_HeightShaping.TerraceSteps` | uint |
| `TerraceSharpness` / `HeightExponent` | `m_HeightShaping.*` | float |
| `AutoMaterial` | `m_AutoMaterial` | bool |
| `SplatmapGenResolution` | `m_SplatmapGenResolution` | uint |

The nested `TerrainHeightShaping` scalars are *flattened* onto the component — the
C# bindings come from custom `OLO_PROPERTY` `Get`/`Set` expressions that forward to
`m_HeightShaping.*`, and the Lua usertype does the same. `m_LayerRules`, the
material/layers, the streaming/voxel config and all `Ref<>` runtime handles stay
editor-only (they don't map to the scalar `OLO_PROPERTY` model).

**The regeneration trigger.** `OLO_PROPERTY` only generates field get/set, so
setting `Seed` alone has no way to *re-run* generation. `TerrainComponent::Regenerate()`
is that trigger — it drops the cached `TerrainData` / chunk manager / streamer and
arms `m_NeedsRebuild` / `m_AutoSplatNeedsRebuild`, so the next
`Scene::ProcessScene3DSharedLogic` tick rebuilds the height field from the current
params (reusing the existing rebuild path — no new generation code). It's bound as
a C# method (`TerrainComponent_Regenerate` internal-call) and a Lua usertype
function (`regenerate`). Set the params first, then call `Regenerate()`; the actual
rebuild happens once, on the next render tick. (Just flipping `m_NeedsRebuild`
would *not* work — the rebuild only regenerates the height field when
`m_TerrainData` is null, which is exactly what `Regenerate()` resets.)

C# ([`TerrainController.cs`](../OloEditor/SandboxProject/Assets/Scripts/Source/TerrainController.cs)):

```csharp
var terrain = GetComponent<TerrainComponent>();
terrain.Seed = new System.Random().Next();  // fresh world each run
terrain.Octaves = 7;
terrain.RidgeBlend = 1.0f;                   // sharp mountains
terrain.HeightScale = 120.0f;
terrain.Regenerate();                        // rebuild on the next tick
```

Lua ([`LuaTerrainController.lua`](../OloEditor/SandboxProject/Assets/Scripts/LuaScripts/LuaTerrainController.lua)):

```lua
local terrain = entity_utils.get_component(entityID, "TerrainComponent")
terrain.seed = 1337
terrain.octaves = 7
terrain.ridgeBlend = 1.0
terrain.heightScale = 120.0
terrain:regenerate()
```

Lua setters validate finiteness / sane ranges (mirroring the `WaterComponent`
usertype); the C# setters inherit the generated `std::isfinite` guard on floats.

A ready-made demo scene,
[`ScriptedTerrainDemo.olo`](../OloEditor/SandboxProject/Assets/Scenes/ScriptedTerrainDemo.olo),
wires both scripts onto two side-by-side procedural terrains (left = Lua, right =
C#); each picks a fresh seed on start and regenerates on the **R** key.

---

## Tests

- **CPU contracts** —
  [`TerrainGeneratorTest.cpp`](../OloEngine/tests/Terrain/TerrainGeneratorTest.cpp)
  (`unit`, runs in CI): determinism, [0,1]/finite range under every shaping
  knob, seed sensitivity, terrace endpoints/monotonicity/plateaus, rule band
  membership + slope selection, weight normalization, the no-match → layer 0
  fallback, and splatmap byte packing.
- **Visual evidence** —
  [`TerrainGenerationEvidenceTest.cpp`](../OloEngine/tests/Rendering/PropertyTests/TerrainGenerationEvidenceTest.cpp)
  (`L8`): renders a generated, auto-materialed terrain through the full editor
  pipeline from an oblique + top-down pose and asserts framing-independent
  *banding* contracts (many distinct colour buckets + a wide luminance spread) —
  i.e. the splatmap actually reached the shader and the terrain is textured, not
  a flat single layer. Writes PNGs to `OloEditor/assets/tests/visual/` under
  `OLOENGINE_GOLDEN_REBASE=1`. SKIPs cleanly without a GL 4.6 context.
- **Script-driven regeneration** —
  [`LuaDrivesTerrainRegenerationTest.cpp`](../OloEngine/tests/Functional/Scripting/LuaDrivesTerrainRegenerationTest.cpp)
  (`Functional`, runs in CI): drives a real `Scene::OnUpdateRuntime` tick where a
  Lua script sets `seed` + `octaves` through the usertype and calls `regenerate()`,
  then asserts the params round-tripped, `regenerate()` dropped the cached
  `TerrainData` + armed the rebuild flags, and the script-set params
  deterministically change the `GenerateHeightField` output (the CPU half of the
  Scene's GPU rebuild — the actual heightmap upload only runs from the render path,
  which Functional tests don't drive). The C# binding surface is generated by
  OloHeaderTool and compiled into `OloEngine-ScriptCore`.

---

## Future work

- **Hydraulic/thermal erosion as a generation stage.** Erosion already exists as
  an editor brush
  ([`TerrainErosion`](../OloEngine/src/OloEngine/Terrain/Editor/TerrainErosion.h));
  running it as an automatic post-pass of `GenerateHeightmap` (with a serialized
  iteration count) would add realistic drainage channels and talus slopes.
- **Biome masks.** A low-frequency biome noise selecting between rule *sets*
  (desert / alpine / temperate) instead of one global rule list.
- **Foliage auto-population from rules.** Foliage already reads splatmap channels
  and slope; a generator preset that emits matching `FoliageLayer`s would make
  "generate a vegetated world" one click.
- **Non-square / streamed generation.** The generator is single-tile; wiring it
  into `TerrainStreamer` would allow infinite procedural worlds.
