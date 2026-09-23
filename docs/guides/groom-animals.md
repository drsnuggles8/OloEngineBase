# Groomed animals and human hair in one scene

`Scenes/GroomAnimals.olo` puts all nine groom children (#1232, #1246–#1253) on three moving subjects
in one scene. It is the live subject of the groom epic's acceptance (#1223).
`GroomAnimalsAcceptanceEvidenceTest` is its headless twin.

## The subjects

| subject | body | coat | motion |
|---|---|---|---|
| short-coated horse | `Models/Horse/Horse.gltf` (CC0) | bay summer coat: undercoat and guard hair, a dark mane, forelock and tail | `Walk` clip |
| long-coated horse | the same body | winter coat: long guard hair over a dense undercoat, leg feathering, heavier mane and tail | `Walk`, half a stride out of phase |
| human | `assets/models/InfiniteScanHead/HeadRigged.gltf` (CC BY 3.0) | dark straight hair, with a swept fringe | `LookAround` clip |

The horses are the same species on purpose. No realistic, rigged, long-coated animal (a dog, wolf or
sheep) is available under a licence this repository accepts without an account-gated download, and a
second body at the Fox's 576 triangles would not show a coat worth judging. Two coats on one body
isolate what the coat authoring does, which is what #1251 is about.

Every coat carries every child's component: `GroomComponent` in `StochasticAlpha` (#1246),
`GroomFibreComponent` (#1247), `GroomCoatShadowComponent` (#1248), `GroomBindingComponent` (#1249),
`GroomSimulationComponent` (#1250), `GroomCoatComponent` (#1251, with the horse's own albedo as the
colour map), and `GroomLodComponent` with a cooked card level (#1252). #1253 needs nothing authored: it
takes part whenever Vulkan RT shadows are armed and a light opts in.

## Opening it

**Turn TAA on.** `StochasticAlpha` needs a temporal resolve, and without one #1246 falls back to its
opaque tier, which cannot draw sub-pixel hair. The scene opens looking bald. TAA is not a per-scene
setting, so the scene cannot ask for it (a follow-up is filed on #1223). In the editor:
Post-processing → TAA, or `olo_postprocess_settings_set {"field":"TAAEnabled","value":true}`.

Press **Play**. The clips and the guide simulation only advance in runtime frames. In edit mode the
coats hold their bind pose, and that is correct.

Expect it to be slow: about 250 ms a frame on an RTX 4090 at full density. That is the measured cost
the epic's third criterion asks about, not a misconfiguration. See *Known gaps*.

## Regenerating it

Everything is generated; nothing is hand-authored.

1. Bodies: `Models/Horse/prepare_horse.py` and `InfiniteScanHead/prepare_head_rig.py` (Blender 5.x,
   headless). Look at the preview frames they write before committing.
2. Coats, bindings, the registry entries and the scene:
   `OLO_GROOM_ANIMALS_EXPORT=1 OloEngine-Tests --gtest_filter=GroomAnimalsAcceptanceEvidenceTest.ExportsTheLiveScene`.
   It grows the coats from the bodies' own bind-pose surfaces, cooks them, imports them through the
   Sandbox project's `EditorAssetManager` (the only writer of `AssetRegistry.oar`), and writes the
   scene with `SceneSerializer`, so every key comes from the reference generator.

The coats are grown in the test, not in a DCC. Roots are sampled on the body's triangles by area,
their UVs come from the body's UVs, and regions come from the rig: the dominant bone of each
triangle, refined by where it faces (the mane crest and the forelock). A strand stands for a lock,
not a hair. At a few strands per square centimetre, a real 0.1 mm hair covers a few percent of the
skin, so the widths are authored to carry coverage.

## What the headless fixture asserts

All on GL, Release, 1280×720.

- **Every child integrates, on every raster path**: three coats deformed, none refused, none held at
  rest, all simulated, all lit, length preserved, no history dropped mid-stride. The #1248 refusal on
  moving coats is asserted as the gap it is. Each coat must cover at least 1% of a frame framed on it.
- **Every child is load-bearing**: each lever switched off on the moving long coat changes the frame
  by more than twice the measured repeat floor.
- **The coat stays on the body through a stride**: eight steps through the walk, with coverage steady
  within ±35%.
- **Coverage near to far**: the LOD ladder keeps the coat's share of the animal within 30% at 5 m and
  14 m. At 32 m, on cards, it is about 1.35×, a measured gap.
- **The temporal resolve settles the coat**: shimmer is less than half the no-history control.
- **Cost across a herd**: 3, 7 and 12 animals, scheduler off and on. GPU pass time, strands, guides
  and resident geometry are written to `GroomAnimals_Cost.txt` against the named GPU.
- **Loose and packed assets are the cooked assets**: a byte-exact round trip, the bindings accept a
  cache-served reload of their bodies, and the scene draws the pack-loaded coats.

## Known gaps

Filed on #1223 as its blockers:
- a moving coat has no self-shadowing;
- the per-frame cost of a deformed coat;
- the card tier over-covers at range;
- the scene is bald without TAA, which it cannot request;
- coats misregister under editor upscale (with #1397).

`GroomAnimals_Cost.txt` and the PR body hold the numbers.
