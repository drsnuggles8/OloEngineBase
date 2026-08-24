# OpenVDB volumetric import (#724) — the editor-only dependency split, and what broke

Companion to [asset-import-usd-alembic.md](asset-import-usd-alembic.md): same "interchange
importer" shape, but this one has a hard constraint USD/Alembic don't — the issue's acceptance
criteria require **zero OpenVDB dependency in the shipped runtime**. Read this before touching
`OloEngine-VolumeCook/`, `Asset/VolumeAsset.h`, `Asset/Serializers/VolumeSerializer.cpp`,
`Serialization/VolumeBinaryFormat.h`, or `FogVolumeComponent`'s `Texture3D` shape.

## The pattern: a new static-lib target, not another `OLO_WITH_*` link on `OloEngine`

USD, Alembic and MaterialX are all linked straight onto the `OloEngine` target (`OloEngine/
CMakeLists.txt`), which every app (`OloEditor`, `OloRuntime`, `OloServer`) links — so they're
*all* carrying those dependencies today, just unused code in the runtime binaries. That's tolerated
for those three; it is explicitly **not** tolerated here.

The fix is a new target, `OloEngine-VolumeCook` (sibling of `OloEngine-LuaScriptCore`), that:

- links `OpenVDB::openvdb` **and** `blosc_static` (see the vcpkg trap below) `PUBLIC`,
- links `OloEngine` `PUBLIC` (needs `Ref<T>`, `Base.h` typedefs, glm),
- is linked onto `OloEditor` and `OloEngine-Tests` **only** — the exact same shape
  `httplib::httplib` already uses in `OloEditor/CMakeLists.txt` (a dependency + its consuming code
  confined to the editor, never touching `OloEngine`). `OloRuntime`/`OloServer` never mention it.

Everything OpenVDB-shaped lives in `OloEngine-VolumeCook/`. `OloEngine` itself only knows the
*cooked* `.olovol` format (`VolumeAsset`, `VolumeSerializer`) — pure binary decode + GPU upload,
no OpenVDB include anywhere in it. `AssetExtensions.cpp` registers `.olovol` unconditionally but
deliberately does **not** register `.vdb` — the source format isn't importable by `OloEngine` at
all; the editor's Content Browser recognizes `.vdb` itself (gated on `OLO_WITH_OPENVDB`) and calls
`OloEngine::VolumeCook::CookOpenVDBToNativeFile` to produce the `.olovol` that `AssetExtensions`
*does* know.

**Verify this by building, not by reading the CMake.** "The target links `OpenVDB::openvdb`" is
not proof the runtime is clean — check `OloRuntime`'s and `OloServer`'s link line has no
`OloEngine-VolumeCook` on it and that a full `OloEngine-Tests` build actually produces the compute
symbols the way you expect (see the blosc trap immediately below, which only fails at the *final
exe* link, not at the point you'd naively suspect).

## Trap: `find_package(OpenVDB)` does not re-export blosc

Linking only `OpenVDB::openvdb` compiles fine and fails at the FINAL executable link with
`undefined symbol: blosc_compress_ctx` (and `blosc_init`, `blosc_decompress_ctx`,
`blosc_set_compressor`, `blosc_cbuffer_sizes`) — not at any earlier point, because a static
archive doesn't resolve symbols against another archive; the undefined refs only surface when
something finally has to produce an executable. This is the same shape as
[vcpkg-dependency-management.md](vcpkg-dependency-management.md)'s "the overlay was found is not
the option reached the consumer" — `find_package(OpenVDB CONFIG REQUIRED)` succeeding proves
nothing about what it re-exports.

Fix: `find_package(blosc CONFIG REQUIRED)` and link the exported target directly. Its vcpkg-config
target name is **`blosc_static`** (readable in `<binaryDir>/vcpkg_installed/<triplet>/share/blosc/
blosc-config.cmake` after a first attempt fails) — not `Blosc::blosc`, not `blosc::blosc`.

## Trap: deriving the grid→world transform by hand-transposing OpenVDB's matrix

OpenVDB's `math::Transform::indexToWorld()` applies as `world = indexPos(row-vector) * M`; GLM's
`mat4 * vec4` is the opposite convention (column-vector, `M * v`). Converting `Mat4d` to `glm::mat4`
by copying `M(row, col)` into `out[col][row]` (or `out[row][col]` — both look plausible) is a coin
flip with no compiler help either way; getting it backwards silently produces a **transposed**
transform that is right on the diagonal and wrong off it, so uniform-scale-only test grids (the
easiest ones to author) don't catch it.

**Sidestep the convention question entirely: derive the matrix from four calls to
`indexToWorld()`, not from reading `Mat4d`'s storage.**

```cpp
auto worldAt = [&](double oi, double oj, double ok) { return xform.indexToWorld({oi, oj, ok}); };
const auto origin = worldAt(0,0,0);
const auto axisX  = worldAt(1,0,0) - origin;   // world-space displacement of ONE unit along output-X
const auto axisY  = worldAt(0,1,0) - origin;
const auto axisZ  = worldAt(0,0,1) - origin;
// glm::mat4 columns ARE where basis vectors map to under M*v — this is now correct by
// construction, independent of whatever internal convention indexToWorld() uses.
out[0] = {axisX, 0}; out[1] = {axisY, 0}; out[2] = {axisZ, 0}; out[3] = {origin, 1};
```

Pin it with a test that calls the SAME `indexToWorld()` independently and compares world positions
at several sample corners — `VolumeImportTest.cpp`'s
`GridTransformReproducesIndexToWorldExactly` is the pattern. It caught nothing here (the
by-construction derivation was right first try), but it is the only thing that would have.

## Extending `FogVolumeShape` hit an exhaustive `switch` two call sites away from every place you'd think to look

Adding `FogVolumeShape::Texture3D = 3` required touching: the enum, `Components.h`'s
`OLO_SERIALIZE(Clamp, Max=...)`, `PostProcessSettings.h`'s GPU-side constant, the
if/else-if chain in `SceneHierarchyPanel.cpp`'s inspector UI, and the new
`evaluateFogVolumesAtPointVDB` GLSL function. None of those crash if you miss one — they degrade
(wrong UI, wrong shading). The one that **does** crash is 5000 lines away in `Scene.cpp`: a
`switch (fogVol.m_Shape)` that draws an editor gizmo (box/sphere/capsule per shape) has a
`default: OLO_CORE_ASSERT(false, "Unknown FogVolumeShape")`. A fresh enumerator with no `case`
trips it the instant a `FogVolumeComponent` with the new shape gets drawn in ANY scene with
gizmos enabled — which for this repo's headless suite means the first
`RunEditorFrames` call in the visual-evidence test, reported as `STATUS_BREAKPOINT` (Windows
exit code `-2147483645` / `0x80000003`), not a normal gtest failure line.

**Generalisable:** `grep -rn "EnumName::" src/` before extending any enum that already has more
than one consumer — an exhaustive `switch`'s `default: assert(false)` is a *good* pattern (loud,
not silent), but only if you find it before the crash does. It will not be next to the other
places you already knew to edit.

## The plain (non-VDB) `evaluateFogVolumesAtPoint()` needed an explicit skip, not just a sibling function

`FogVolumeCommon.glsl` is `#include`d by more than the froxel-fog compute shader (the analytical
fog fallback and `VolumetricShadow_Generate.comp` also use it), and those callers have no
`sampler3D` to bind. The instinct is "add a new function for the VDB-aware case, leave the old one
alone" — but the old one's `evaluateVolumeSDF()` has an `else // BOX (default)` fallthrough, so a
`Texture3D`-shaped volume silently rendered as a **solid opaque box** through every caller that
didn't get the new function. `FogSettings::EnableVolumetric` defaults `false`, so the analytical
(non-VDB) path is the *default* fog path in any scene that hasn't explicitly turned on froxel fog —
this was not an edge case. Fix: the plain evaluator explicitly `continue`s on
`FOG_VOLUME_SHAPE_TEXTURE3D` (contributes nothing) rather than falling into the SDF default.
Caught in self-review, not by any test — the visual-evidence test only exercises
`EnableVolumetric = true`.

## Froxel fog's temporal history ghosts a just-disabled volume for several frames

`VolumetricFogPass` reprojects/accumulates its scatter volume across frames
(`m_ScatterVolume` ping-pong, `m_HistoryValid`). Toggling `FogVolumeComponent::m_Enabled` off and
rendering 2 frames (the settle count other temporal effects in this suite use) still showed a
faint ghost of the volume in the "off" capture — it took 8 frames to converge cleanly. Not
necessarily a product bug (a one-or-few-frame fade on disable is normal for a temporally-denoised
effect), but a real trap for **differential visual tests that toggle state and expect an
immediate clean before/after** — budget enough settle frames, and don't assume 2 is universal
just because it's enough elsewhere.
