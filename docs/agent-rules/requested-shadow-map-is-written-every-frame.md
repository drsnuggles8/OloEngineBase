# A requested shadow map is written every frame

Read before adding an early-out to `ShadowRenderPass::Execute`, a "skip this cascade / layer /
tile" test to any shadow technique, or any other pass whose output a later pass samples whether or
not this one ran.

## The rules

1. **If a light requested shadows this frame, write its map this frame, casters or not.** Its
   receivers sample the map whatever the shadow pass did. "Nothing to cast" has to produce a map
   that says so: cleared to far depth, which reads as lit. The only frames to skip are the ones
   nobody samples: shadows off, or no light asking (`ShadowMap::AnyShadowsRequested`).

2. **A cascade with no caster in it is cleared, not skipped.** When
   `ShadowMap::IsDirectionalShadowRequested()` every cascade is an item of the CSM region and gets
   its attach and its depth clear. An empty cascade costs that much and no draw.

3. **Test the frame after the last caster leaves, not only the frames that have one.** A scene with
   a casting ground plane in every cascade cannot show this, and almost every scene and test has
   one. `ShadowMapWithoutCastersTest.cpp` has none: the caster is deleted, moved outside every
   cascade, or was never there, on Forward, Forward+ and Deferred, on the cascades and on the
   virtual shadow map's pages, and on both backends.

## What it cost

`ShadowRenderPass::Execute` returned when every caster list was empty, and its cascade loop
`continue`d past any cascade no caster reached. Both were written as savings. The map then kept
what it held:

- the shadow of a caster that had just been deleted or had walked away, frozen on the ground;
- the previous scene's shadows after a scene change;
- in a process that had never drawn a caster, the texture's initial zeroes, under which every
  receiver of the sun is fully shadowed.

It surfaced as the third. The evidence scene for #1691 has a sun and two fields of cubes that do not
cast, and it rendered at 46 of 255 where the same scene with the sun's shadows off rendered at 129.
The log said `ShadowRenderPass::Execute skipped: no shadow casters submitted`, once. With the fix
removed, all nine cascade cases of the test fail: the deleted caster's shadow is still 69 levels
darker than the lit ground beside it.

Virtual shadow maps had the same hole one level up: the page work that retires a departed caster's
shadow ran only if the pass ran, and the pass did not run on the frame the last caster left.
