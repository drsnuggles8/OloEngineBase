# Groom surface binding

Issue #1249. Attaching a cooked groom to an animated body so the coat follows skeletal bending and facial morphs. Applies to `Groom/GroomBinding*`, `GroomSurfaceFrame.h`, `GroomDeformation.*`, `GroomBindingCooker.*`, `Scene::DeformGroomAgainstSurface` and the groom pass's per-entity dynamic geometry.

## Rules

1. **Refuse on a topology mismatch; never rebind, re-snap or fall back.** A binding addresses triangles by index, and a binding attached to another body with the same vertex count gives a plausible wrong coat. `CheckCompatibility` returns the first reason; the caller reports it and draws the coat at rest. [Details](groom-surface-binding-details.md#the-rules)
2. **Build the frame in exactly one place,** `MakeGroomSurfaceFrame`, for both the bind-time record and per-frame evaluation. A second construction that disagrees on handedness or tangent edge rotates every strand. [Details](groom-surface-binding-details.md#the-rules)
3. **Carry each strand rigidly from its root's frame;** do not skin points against the body. Rigid transfer preserves length; per-point skinning pinches long strands. [Details](groom-surface-binding-details.md#the-rules)
4. **Store the rest frame in the binding; never recompute it from the live mesh.** Morph targets are applied on the CPU into `MeshSource`'s vertex array, so readable "rest" positions are the morphed ones and the coat would drift with each expression. [Details](groom-surface-binding-details.md#the-rules)
5. **Split the topology check from the bind-pose check.** Vertex count, index count, index hash, bone count and skeleton identity are morph-invariant and checked on every attach (`GroomBindingTargetSignature::MatchesTopology`); the bind-pose position hash is build/editor-time only. [Details](groom-surface-binding-details.md#the-rules)
6. **Derive previous-frame positions from the previous pose,** `SkeletonData::m_PrevFinalBoneMatrices`, not a carried-forward buffer; hold `prev == current` and the frame emits exactly zero motion. [Details](groom-surface-binding-details.md#the-rules)
7. **Require both history gates:** the caller's (teleport, LOD switch, morph state moved) and the skeleton's `HasBoneHistory()`. Either alone shows as a coat-length TAA smear. [Details](groom-surface-binding-details.md#the-rules)
8. **Attribute every reset** with `GroomHistoryResetCause` via `GroomHistoryCauseFor`, so an unloaded body does not report a changed binding. [Details](groom-surface-binding-details.md#the-rules)
9. **Bump the right version.** `OloGroomBindingFormat::CurrentVersion` moves when the bytes move ("file is corrupt"); `kGroomBinderVersion` moves when the binder's output moves for unchanged input ("rebuild the binding"). [Details](groom-surface-binding-details.md#the-rules)
10. **Do not bump `kSaveGameFormatVersion` for a new component;** save components are keyed by an FNV hash of the type name, and a bump collides with other branches appending to existing components. [Details](groom-surface-binding-details.md#the-rules)
11. **Feed the previous `SurfaceToGroom` too.** `GroomDeformationInputs::PrevSurfaceToGroom` is an `optional` whose unset state means the same matrix, not identity; mixing last frame's palette with this frame's matrix smears walking characters while standing ones look fine. [Details](groom-surface-binding-details.md#the-rules)
12. **Make the curve selection exact at both ends.** `SelectGroomStrandCurves` can return nothing, so the parameter is `std::optional<std::span<const u32>>` and an empty span means none, not everything. The deformer's cap must match the build's: the curve that straddles the cap is on both lists and every one after is on neither; a budget under one strand's length builds one strand. [Details](groom-surface-binding-details.md#the-rules)
13. **Bound the ribbon, not the centreline:** expand by `Radius` and include the previous positions (the motion-vector pass reads them), or a culler removes visible strands. [Details](groom-surface-binding-details.md#the-rules)
14. **Cook through `FileSystem::WriteFileAtomically`,** which writes a temporary, checks it after `close()` (a full disk reports at the flush) and renames it over the target. Truncating a working binding first turns a failed write into a broken groom. [Details](groom-surface-binding-details.md#the-rules)
15. **Put identities in cooked filenames.** Two bodies may both be called "Body"; sanitise names (a `:` on Windows writes an NTFS alternate data stream) and append a hash of the groom handle and target UUID. [Details](groom-surface-binding-details.md#the-rules)
16. **When `GroomStrandVertex` grows, move together in one commit:** the shader's pull stride, the GL attribute layout and the `sizeof` pin; append new floats so existing offsets hold. The stride went 12 to 16 for `PrevPosition` (Vulkan pulls a flat float array, ADR 0011 §5); an unbound groom writes `PrevPosition == Position`. [Details](groom-surface-binding-details.md#the-vertex-grew-and-the-vulkan-pull-did-not)
17. **Break nearest-triangle ties by the lowest triangle index** with a strict `<` against the running best and index-sorted cell lists, so grid cell size cannot change the result (`GroomBindingBuilder.AGridResolutionChangeDoesNotChangeTheResult`, `.ATieBetweenTwoTrianglesGoesToTheLowerIndex`). [Details](groom-surface-binding-details.md#the-tie-that-was-resolved-by-cell-size)
18. **`static_assert` the size of every block that goes to disk,** and compute it: `GroomRootBinding` is 56 bytes, not 48. [Details](groom-surface-binding-details.md#56-bytes-not-48)
19. **Do not add simulation or a second body-skinning system here.** Simulation is #1250 (strands are rigid to their root by design); the deformation evaluates #1226's shared output at three vertices per strand root (`skeletal-deformation-shared-output.md`). [Details](groom-surface-binding-details.md#what-is-deliberately-not-here)

## Where to look

`Groom/GroomBinding.h` (asset, signatures, refusals), `GroomSurfaceFrame.h` (the one frame), `GroomBindingBuilder.{h,cpp}`, `GroomDeformation.{h,cpp}`, `GroomBindingCooker.{h,cpp}`, `Scene/Scene.cpp::DeformGroomAgainstSurface`, `Renderer/Passes/GroomRenderPass.cpp`.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[groom-surface-binding-details.md](groom-surface-binding-details.md).
