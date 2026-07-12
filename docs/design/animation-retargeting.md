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

## Humanoid bone-enum mapping

Name matching cannot relate bones whose names share nothing — 3ds Max biped
`Bip01 L UpperArm`, Unreal `upperarm_l`, and Mixamo `LeftArm` all denote the same
joint but normalize to three different strings. The humanoid-role layer bridges
them by anatomy instead of name:

- **`HumanoidBone`** ([HumanoidBone.h](../OloEngine/src/OloEngine/Animation/Retargeting/HumanoidBone.h))
  — a canonical role enum (Hips, Spine, Chest, Neck, Head, and the L/R arm and leg
  chains down to a single toe joint), modeled on Unity's `HumanBodyBones` / UE's
  humanoid rig. Pragmatic, not exhaustive: no individual fingers, eyes, or jaw.
- **`HumanoidBoneMap`** ([HumanoidBoneMap.h](../OloEngine/src/OloEngine/Animation/Retargeting/HumanoidBoneMap.h))
  — one skeleton's bone→role assignment. `AutoDetect` fills it heuristically from
  the bone names, recognizing the four common conventions (Mixamo, Unreal, 3ds Max
  Biped, Blender/Rigify): it tokenizes each name (stripping rig prefixes, splitting
  camelCase and letter/digit boundaries), detects a left/right side, and matches a
  body-part keyword. It folds a multi-bone spine onto Spine (lowest) + Chest
  (highest), and rejects finger/twist/IK helper bones. `SetBone` overrides a
  mis-detected or missed role by hand. `ClassifyBoneName` exposes the single-name
  classifier for tests/tooling.
- **`SkeletonRetargetMap::BuildByHumanoidRole`** pairs a source and target
  `HumanoidBoneMap` role-by-role into the same per-target-bone table the rest of the
  pipeline consumes — so the rebasing math, clip baking, and `AnimationSystem`
  play-out are all **unchanged**; only the *correspondence* source differs. A
  two-argument overload auto-detects both rigs. Compose role + name with
  **`FillUnmappedFrom`**: `roleMap.FillUnmappedFrom(SkeletonRetargetMap::BuildByName(src, tgt))`
  keeps every role-mapped bone and falls back to name matching for the rest
  (e.g. a same-named non-humanoid `Tail`).

### Why a clip-bake (and no ECS component) for slice 1

The integration deliberately avoids adding an ECS component (which would pull in
the six cross-binding touch-points — `AllComponents` tuple, `OnComponentAdded`,
scene + save-game serialization, scripting bindings). Baking a target-ready clip
keeps the whole slice at the skeleton/clip API plus a CPU test. Author-facing
authoring (pick a retarget source in the editor) is a follow-up that *will* need a
component.

## Deferred (follow-ups)

All four follow-ups have now shipped:

1. ~~**Humanoid bone-enum mapping**~~ — **done** (see "Humanoid bone-enum mapping"
   above): `HumanoidBone` + `HumanoidBoneMap::AutoDetect` +
   `SkeletonRetargetMap::BuildByHumanoidRole`. The auto-mapper is
   heuristic — Blender/Rigify's numbered-spine metarig is best-effort, and an
   unconventional rig may need `SetBone` overrides.
2. ~~**Full per-bone translation retargeting**~~ — **done** (issue #631 part 2):
   `RetargetOptions::TranslationMode::PerBoneRatio` transfers every mapped
   non-root bone's translation delta scaled by the target/source bone-length
   ratio (`|targetRest.Translation| / |sourceRest.Translation|`, falling back to
   `RootTranslationScale` for a bone resting at its parent origin). Available in
   both `RetargetLocalPose` and the `RetargetClip` bake; default stays
   `RootOnly` (the first-slice behavior). IK-preserving foot/hand placement is
   handled downstream by `FootIKComponent` (docs/design/locomotion.md).
3. ~~**Live (un-baked) runtime retargeting**~~ — **done** (issue #631 part 2):
   `RetargetingComponent` (`Animation/Retargeting/RetargetingComponent.h`) +
   `RetargetingSystem`, running as the `"Retargeting"` gameplay-scheduler node
   before the animation systems (and in the editor-preview tick). The source rig
   + clips come from a scene entity (`m_SourceEntity`) or a model file
   (`m_SourcePath`); the system lazily retargets every source clip onto the
   entity's skeleton and splices the results into
   `AnimationStateComponent::m_AvailableClips` under their source names — so
   plain clip playback, animation graphs (`ResolveClips` by name) and
   root-motion extraction (the baked clip carries the source's
   `RootMotion` settings with the root index remapped) all work unchanged. It
   rebakes only when the authored settings change (settings-equality check
   against the runtime `RetargetingStateComponent` cache). Full cross-binding:
   generated tuple/serializers, SaveGame `Serialize()`+`RegisterAll`, Lua
   usertype, `OLO_PROPERTY` C# glue, editor DrawComponent undo.
4. ~~**Editor authoring UI**~~ — **done** (issue #631 part 2): the
   "Animation Retargeting" inspector block (SceneHierarchyPanel) — source
   entity/model pickers (drag-drop `CONTENT_BROWSER_MODEL`), mode toggles, and a
   "Target Role Assignments" table showing each bone's `AutoDetect` result with
   a per-bone override combo (stored in `m_TargetRoleOverrides`, `*` marks a
   hand-correction). Live preview: the bake also runs in edit mode, so playing a
   baked clip in the Animation panel previews the retarget without entering
   Play.

## Tests

`AnimationRetargetTest.cpp` (classified `unit`, alongside the IK solver tests —
it's a direct-API CPU contract test, not a `Scene::OnUpdateRuntime`-driven
Functional test) covers: name normalization, exact/normalized mapping precedence,
rotation transfer with preserved proportions, the rest-orientation delta re-base,
scaled root-translation transfer, and the end-to-end baked-clip → `AnimationSystem`
path on a differently-proportioned target rig. For the humanoid-role layer it adds:
single-name classification across all four naming conventions (and rejection of
finger/twist/IK/sideless bones), `AutoDetect` over a full skeleton with the
Spine/Chest collapse, `BuildByHumanoidRole` mapping two rigs whose names share
*nothing* (where `BuildByName` maps zero bones), the explicit-override +
`FillUnmappedFrom` name-fallback compose, and the end-to-end role-mapped clip bake
driving a disjointly-named target through `AnimationSystem::Update`.
