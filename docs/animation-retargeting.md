# Animation retargeting

Applying an `AnimationClip` authored for one skeleton onto a *differently-rigged*
skeleton (bone remapping / rig retargeting). Code lives in
[OloEngine/src/OloEngine/Animation/Retargeting/](../OloEngine/src/OloEngine/Animation/Retargeting/);
the CPU contract test is
[OloEngine/tests/Animation/AnimationRetargetTest.cpp](../OloEngine/tests/Animation/AnimationRetargetTest.cpp).

## What ships in the first slice

Rotation-based, name-mapped retargeting:

- **`SkeletonRetargetMap`** — a per-target-bone → source-bone table, built from two
  skeletons by matching bone **names**. An exact-name pass runs first; unmatched
  target bones then fall back to a **normalized** match (`NormalizeBoneName`: strip
  the namespace/rig prefix up to the last `:` or `|`, drop non-alphanumerics,
  lower-case), so `mixamorig:LeftArm` ↔ `Left_Arm` ↔ `left arm` all resolve.
  Mappings can also be set/overridden by hand with `SetBoneMapping`.
- **`AnimationRetargeter::RetargetLocalPose`** — the runtime primitive. For each
  mapped target bone it transfers the source bone's animated rotation expressed as
  a **delta from the source rest pose**, re-based onto the **target rest pose**:
  `R_target = R_target_rest · (R_source_rest⁻¹ · R_source_anim)`. With both rest
  poses identity this degenerates to a direct rotation copy; with differing rest
  orientations it stays correct (it is *not* a naive local-rotation copy).
  Translations and scales are taken from the **target** rest pose, so the target
  keeps its own proportions. The **root** bone's translation is optionally
  transferred (scaled) so locomotion / hip motion carries across.
- **`AnimationRetargeter::RetargetClip`** — bakes a source clip into a **new**
  `AnimationClip` whose tracks are named for the target's bones, with rotation keys
  re-based and translation/scale keys holding the target rest values (root
  translation optionally transferred). This is the integration path: bake once,
  then play the result on the target through the normal `AnimationSystem::Update`
  with no per-frame retarget cost and no new ECS component.
- **`AnimationRetargeter::ComputeRootTranslationScale`** — derives a uniform
  root-translation scale from the ratio of the two rigs' bind-pose extents.

### Why a clip-bake (and no ECS component) for slice 1

The integration deliberately avoids adding an ECS component (which would pull in
the six cross-binding touch-points — `AllComponents` tuple, `OnComponentAdded`,
scene + save-game serialization, scripting bindings). Baking a target-ready clip
keeps the whole slice at the skeleton/clip API plus a CPU test. Author-facing
authoring (pick a retarget source in the editor) is a follow-up that *will* need a
component.

## Deferred (follow-ups)

Tracked so the next pass knows what's left:

1. **Humanoid bone-enum mapping** — map anatomically equivalent bones whose names
   share nothing (3ds Max biped `Bip01 L UpperArm` ↔ UE `upperarm_l`) via a
   canonical humanoid-bone enum, not just name normalization.
2. **Full per-bone translation retargeting** — only the root translation is
   transferred today; non-root bones take the target rest translation. A
   bone-length-ratio scheme for limbs (and IK-preserving foot/hand placement) is
   future work.
3. **Live (un-baked) runtime retargeting** — a retarget source/option on the
   clip-play path, driven by an ECS component, so the source clip need not be
   baked ahead of time. This is where the cross-binding component work lands.
4. **Editor authoring UI** — choosing a retarget source skeleton/clip per animated
   entity, with a preview.

## Tests

`AnimationRetargetTest.cpp` (classified `unit`, alongside the IK solver tests —
it's a direct-API CPU contract test, not a `Scene::OnUpdateRuntime`-driven
Functional test) covers: name normalization, exact/normalized mapping precedence,
rotation transfer with preserved proportions, the rest-orientation delta re-base,
scaled root-translation transfer, and the end-to-end baked-clip → `AnimationSystem`
path on a differently-proportioned target rig.
