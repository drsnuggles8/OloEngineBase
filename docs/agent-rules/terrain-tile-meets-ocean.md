# A terrain tile meeting an ocean — the border is the bug, and it is not in the frame

Applies to: `OloEngine/src/OloEngine/Terrain/TerrainGenerator.{h,cpp}`, any scene that
puts a `TerrainComponent` and a `WaterComponent` in the same world.

Written from issue #880 (*Drift*'s procedural island field). The defect had shipped
in a scene for two issues, was visible in every screenshot anyone had taken of it,
and nobody had named it — because it looks like scenery until you measure it.

---

## 1. Every knob was uniform, so none of them could fix a radial problem

`TerrainGenerator` shapes a tile with ridge blend, domain warp, terracing, a height
exponent and hydraulic erosion. All five are **uniform over the tile**: they are
functions of the height value, or of a noise field sampled the same way everywhere.
None of them knows where the tile's *edge* is.

A tile is a square. An ocean cuts it at a horizontal plane. Wherever the noise
happens to be high at the tile **border**, the terrain simply stops — a vertical
wall of ground dropping into the sea at the tile boundary. That is the "hard seam at
the waterline" `CLAUDE.md` warns about, and it is not a shader problem, a blend
problem or a depth problem. It is the height field.

Measured on Drift's island as it shipped (seed 879, 512², ridge 0.5, warp 0.16,
exponent 1.15, sea level at 0.583 of the height range):

| | |
|---|---|
| tile border above sea level | **64.9 %** |
| highest border texel | 0.954 of the range |

**The trap is that the exponent looks like it fixes it.** Raising `HeightExponent`
does reduce the fraction of border above water — but it reduces *everything* at the
same rate, because it is uniform:

| exponent | sea cut | land | border above water |
|---|---|---|---|
| 1.15 | 0.583 | 63.5 % | 64.9 % |
| 2.00 | 0.583 | 38.0 % | 39.3 % |
| 3.00 | 0.700 | 7.2 % | 7.6 % |
| 3.00 | 0.800 | **1.4 %** | **1.1 %** |

Land tracks border, to within a point, at every setting. By the time the wall is
almost gone the "island" is 1.4 % of the tile — a pebble. There is no parameter set
that produces an island *and* a clean edge, and no amount of tuning will find one.
The fix has to be a **radial** term, which is what `TerrainHeightShaping::
IslandFalloff` is (#880).

Generalise it: when a defect lives at a *boundary* and every available knob is a
function of the *value*, tuning is not a slow path to the fix — it is not a path to
the fix.

## 2. Measure at the border. It takes three seconds and it decides the design

`GenerateHeightField` depends on nothing but `SimplexNoise3D`, `glm` and the standard
library, so it can be transcribed into a standalone binary and answered before any
engine code is written:

```powershell
# stub dir: an empty OloEnginePCH.h plus a Core/Base.h with the scalar typedefs
& "C:\Program Files\LLVM\bin\clang-cl.exe" /std:c++20 /O2 /EHsc /nologo `
  "/I<stub>" "/I<repo>\OloEngine\src" "/I<repo>\build-cached\vcpkg_installed\x64-windows-static-md\include" `
  probe.cpp "<repo>\OloEngine\src\OloEngine\Particle\SimplexNoise.cpp" /Fe:probe.exe
```

The probe reports, per parameter set: land fraction, the fraction of the tile's
outermost ring above a given cut, the highest border texel, and a 44×44 ASCII map of
the coastline. Both tables in §1 came out of it, and so did the six shipped island
configurations — chosen by *looking at the maps*, not by loading the editor six
times. Every number above was measured before the first line of `IslandFalloff`
existed, which is what made "this needs an engine field" an argument rather than a
preference.

(Do this in Git Bash and `/O2` becomes `C:\Program Files\Git\O2.obj`. MSYS path
conversion mangles `/`-style flags — run clang-cl from PowerShell.)

## 3. The ramp must end on the outermost SAMPLE, not on the tile's geometric edge

A radial mask that reaches zero at normalized radius `0.5` — the tile's geometric
half-width — leaves the mid-edge **texel centres** at `0.5 - 0.5/resolution`, just
inside the ramp. The mask there is not zero; it is a small positive tail. On a 120 m
height scale that is centimetres, so it will never be visible, and it will also never
be *provable*: the natural test (`border == 0`) has to be written with a tolerance,
and a tolerance is where a real regression later hides.

End the ramp at `0.5 - 0.5/resolution` instead. Every border texel then lands on
exactly zero, corners included (they sit at ~0.707), and the test is an equality:

```cpp
EXPECT_FLOAT_EQ(MaxBorderHeight(heights, resolution), 0.0f);
```

`TerrainGeneratorTest.IslandFalloffDrivesEveryBorderTexelToZero` sweeps
`{32, 64, 127, 128}` for this reason — the outer radius is resolution-derived, so an
off-by-half-a-texel is invisible at one resolution and not another. It also asserts
the peak survives, because a mask that flattened the whole tile would satisfy the
border check perfectly.

## 4. Apply the mask BEFORE the exponent and the terrace, not after

Both post-normalization passes map 0 to 0 (`pow(0, e) == 0`, and `Terrace(0) == 0` —
check that, it is not free: it holds because the terrace's smoothstep argument
clamps). So the border guarantee survives them either way, and applying the mask
first is strictly better: the exponent and the terrace then shape the **island's own
profile** instead of re-shaping the mask's ramp. A terraced island keeps flat
plateaus; a terraced *ramp* is a set of concentric steps in the water.

If you ever add a shaping pass that does **not** map 0 to 0, the mask has to move
after it, and the border test above is what will tell you.

**The erosion post-pass is exactly that case, and it already existed.**
`ApplyErosion` runs after everything, and a droplet that dies on the border
deposits its sediment there — on precisely the texels the mask drove to the
floor. It is not a shaping function of the height value, so "maps 0 to 0" does
not apply to it. `GenerateHeightField` therefore re-applies the mask factor
after erosion, on the texels where the mask is **zero** and nowhere else: the
ramp and the interior are left as erosion left them, because reshaping a slope
is what erosion is for and re-multiplying the ramp would scale it twice.

The general shape: a guarantee established mid-pipeline is only a guarantee
until the next pass that does not respect it. Enumerate what runs after, and
pin each one with its own case — `IslandFalloffSurvivesTheErosionPostPass`
exists separately from the resolution sweep for that reason, and it asserts
that erosion actually changed the field, or it would be asserting nothing.

## 5. The seafloor plane must sit BELOW the tile base — and this bites after the fix

With the mask on, a tile's border sits at exactly the entity's `y`. A seafloor plane
*above* that height slices every island's underwater flank into a flat horizontal
lid: the island stops at a hard line instead of shelving away.

That is the same artefact you just removed, one subsystem over, and it appears only
*after* the mask starts working — before it, the border was above water and never
touched the seafloor at all. Drift puts the seafloor at `y = -26` and every island
base at `y = -24`; 2 m of clearance at ~24 m depth is invisible through the
underwater fog and leaves no z-fighting.

## 6. Widening the sea silently coarsens it — the grid is a COUNT, the quad is the knob

`WaterComponent` stores `WorldSizeX/Z` and `GridResolutionX/Z` independently, so
growing the tile without growing the count changes the thing that actually matters:
metres per quad. Drift's sea went from 1 km to 1.6 km for the island field; at the
authored 384 that would have quietly gone from 2.6 m per quad to 4.2 m — and the
scene's own notes record that quad size against wavelength is what decides whether
the submerged half of the hull comes through the refraction as a stippled ghost.
640 across 1.6 km restores 2.5 m.

Any time you resize a `WaterComponent`, restate the quad size, not the count. The
same shape applies to `TerrainComponent`'s `ProceduralResolution` against
`WorldSizeX`, and to the splatmap against both.

## 7. `TessellationEnabled` is still a flag scenes forget

[terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md) §1 is about a gating flag
no scene set. `TerrainGpuLodTest.olo` was created to be that scene — and #880 found
that the *game* scene still did not set it, so the island the player actually sails
toward was not using the GPU LOD quadtree at all. Every island in Drift now sets it.

The grep is cheap and worth doing whenever you author a terrain:

```bash
grep -rn "TessellationEnabled: true" OloEditor/SandboxProject/Assets/Scenes/
```

`UseImpostor` was in the same state for foliage — **no** shipped scene set it before
#880, so the octahedral impostor path (#433) had never run outside its own evidence
test. If a feature's flag appears only in the test that was written with it, the
feature has test coverage and no product coverage, and those are different things.

---

## 8. The mask you added to make a shoreline is also a ~50 deg slope, and anything keyed on slope now sees it

Issue #942, straight out of §1–§4 above. The radial mask is what gives the tile a
coastline — and it produces that coastline by ramping the full normalized height
down to zero between `IslandFalloffRadius` and the inscribed circle. That ramp is
not a detail of the shoreline; it **is the entire outer flank of the island**, and
it has a slope you can read off the two numbers you already authored:

```
ramp slope ~= atan(HeightScale / ((0.5 - IslandFalloffRadius) * WorldSizeX))
```

Ridgeback: `atan(110 / ((0.5 - 0.3) * 420))` = `atan(110 / 84)` = **53 deg**. Every
island in Drift lands between roughly 45 and 55 deg by construction. Nobody chose
that; it fell out of the mask.

The auto-material's `TerrainLayerRule` bands are expressed in degrees. Drift's rock
rule opened at `MinSlopeDeg: 48` and spanned the whole height range — *below* the
number the mask guarantees. So rock claimed the flank: **77% of Ridgeback, 86% of
Sisters, 93% of Stacks**, and because `EvaluateLayerWeights` normalizes, every
altitude-banded layer was divided down to nothing. Six islands rendered as one flat
colour each.

Two things make this hard to see:

* **Every stage of the pipeline is correct.** The gate fires, the rules survive
  deserialization, the splatmap generates, uploads, and is sampled and blended
  faithfully. We confirmed all of it — log line, a shader that wrote the raw splat
  sample to `o_Color`, and a second one that wrote the pre-lighting `albedo`. The
  albedo tracked the splat exactly. There is no broken stage to find, which is
  where a whole day can go.
* **The obvious test passes.** "Assert the weights VARY across the height/slope
  range" — they varied fine. What was false is **coverage**: one layer took the
  surface and the others got ~0%. Pin coverage, not variation. That is what
  `DriftIslandSplatCoverageTest` does, and it reads the shipped scene so the scene
  and the guard cannot drift.

### The second half: bands anchored on a range the field never reaches

The mask multiplies the *normalized* field, and `HeightExponent > 1` compresses it
again. So the shaped field no longer reaches 1.0 — Mesa peaks at **0.800**. Drift's
summit bands started at 0.844–0.856, i.e. **above the summit**, and layer 3 drew on
0.0–3.5% of every island. The rule is valid, it simply never matches a texel, and
nothing warns.

Anchor altitude bands on the measured `[min, max]` of the shaped field, not on
`[0, 1]`. `GenerateHeightmap` already logs the range (`height=[...]`) — read it.

### Authoring rule that falls out of both

Give an island **one** cliff threshold and share it: the altitude bands all stop at
theta, the bare-rock rule starts at theta, same `SlopeBlend`. Three unrelated slope
numbers (Drift had 34 / 42 / 48) leave gaps that fall through to the "no rule
matched → layer 0" fallback and overlaps where one rule quietly shadows another.
Pick theta from the island's own slope distribution — the steepest ~25-30% — not
from a default copied off another terrain.

### A float trap on terraced fields

`ShapingTerraceSteps` quantizes the height field onto flat plateaus, so a third of
the surface can share one height value to several decimals. Put a band edge *on*
that value and the winning layer is decided by the last bit: our f64 tuning model
and the engine's f32 `SmoothBand` chose **opposite** layers across 36% of Mesa
(28/31/28/13 predicted, 58/14/28/0 actual). Keep band edges in the quiet parts of
the height histogram, and if you model this offline, model it in **float32**.

### Coverage is necessary, not sufficient — the palette has to survive the air

Fixing the bands got every layer onto a real share of each island and the frame
still read as one brown mass, because that is only half of "can a viewer tell
these apart". Every island also shipped a **pair of materials separated only by
brightness**: Rock/Heath 0.135 apart in RGB, Palm scrub/Green cap 0.075,
Shingle/Lichen 0.134, Dry scrub/Sandstone 0.154. On Ridgeback that pair was 47%
of the surface.

Lightness is the wrong axis to separate on *in this scene specifically*, and the
reason is in the scene: a warm sun at intensity 3.2, exponential fog at density
0.0009 (~30% blend at 400 m), and an ACES curve. All three compress luminance and
pull saturation. Hue is what survives them. Re-separating each pair by hue —
cool grey cliff vs russet heath, yellow-green scrub vs blue-green canopy — moved
the measured hue spread of a framed Ridgeback shot from 0.007 (original) to 0.017
(bands fixed) to **0.034** (bands + palette), with distinct colour buckets
20 → 21 → 24.

The guard asserts plain RGB distance rather than hue, because a big lightness gap
genuinely is distinguishable: Stacks pairs Shingle with Guano, both neutral greys,
0.61 apart and never at risk. What fails is a pair that is close on *both* axes,
and distance alone catches that. Threshold 0.18; the shipped palette's worst pair
is 0.223.
