# Flaky `FogVisualEvidenceTest` — Fog UBO binding knock-out (#446)

> Issue: [drsnuggles8/OloEngineBase#446](https://github.com/drsnuggles8/OloEngineBase/issues/446)
> Symptom: `FogVisualEvidenceTest.DistantGeometryFogsNearStaysClear`
> (`OloEngine/tests/Rendering/PropertyTests/FogVisualEvidenceTest.cpp`) **passes in
> isolation but fails in the full test suite**. Fog ON and fog OFF produce a
> **byte-identical** far band — fog is silently not applied.
> Status: **RESOLVED.** Root cause confirmed by GPU read-back; fix + deterministic
> regression test landed.

## TL;DR

A persistent UBO that is `glBindBufferBase`'d **once in its constructor** and
thereafter only `SetData`'d can have its binding slot silently reverted to **0**
by ordinary cross-test GPU buffer churn (deleting *any* buffer GL has bound at a
slot reverts that slot to 0). The fog pass's `FogData` UBO (binding 17) hit this:
in the full suite it read an **unbound buffer → all zeros → `u_FogFlags.x == 0` →
the shader's "fog disabled" early-out → no fog**, byte-identical with OFF.
Isolation never accumulated enough buffer churn to knock the slot out.

**Fix:** re-bind the `Fog` (17) and `FogVolumes` (20) UBOs after every per-frame
`SetData`, exactly as the motion-blur UBO already does for binding 8
(`RenderPipeline.cpp`). One line each.

## 1. Symptom & reproduction

```
# Fails (cumulative cross-test state):
OloEngine-Tests.exe                                       # full suite
  FogVisualEvidenceTest.cpp(317): Expected: (farOn.B) > (farOff.B + 15.0),
    actual: 138.0077... vs 153.0077...      # farOn.B == farOff.B exactly

# Passes:
OloEngine-Tests.exe --gtest_filter='FogVisualEvidenceTest.*'
```

`farOn.B == farOff.B` to 14 decimals ⇒ the fog-ON capture applied **no** fog at
the far band. The near band was correct (unfogged) in both runs.

## 2. The decisive narrowing (why the reporter's theory was wrong)

In analytical (non-volumetric) mode the fog pass output reduces to a pure function
of `dist = distance(worldPos, cameraPos)` and `density`, where `worldPos` is
reconstructed from scene depth (binding 19) + inverse-VP (binding 8). The reporter
verified depth and inverse-VP are **identical** between the passing/failing runs,
so `worldPos` is pinned.

The reporter's leading theory was a **stale camera position** (binding 0). That is
**geometrically impossible** here: to drop the far band to ~40% fog a displaced
camera would have to sit ~36 units from the back wall, but that same camera would
then be ~130 units from the near floor and **flood the near band** — which stayed
clear. By the triangle inequality no single camera position can be ~36 from the far
geometry and ≲15 from the near geometry when they are ~165 apart. So the only single
variable that reproduces **both** bands (near-clear + far-underfogged) is a globally
**reduced effective fog strength**, i.e. the `FogData` UBO (binding 17) — which the
reporter had **not** read back.

## 3. Confirmation (GPU read-back instrumentation)

Temporary diagnostic in `FogRenderPass::Execute` (gated by `OLO_FOGDBG`) read back
the **live UBO binding ids** and contents the fog draw would see:

```
glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, slot, &id);   // 0 ⇒ slot knocked out
```

| | binding 0 (camera) | binding 8 (inverse-VP) | binding 17 (FogData) |
| --- | --- | --- | --- |
| **isolation (pass)** | id 66 | id 38 | **id 46** — `density=0.02, flags=(1,2,0,0)` ✓ |
| **full suite (fail)** | id 66 | id 38 | **id 0 — UNBOUND** → reads all zeros |

That is the whole bug: binding 17 is unbound in the full suite. The fog shader reads
`FogData` as zeros, `u_FogFlags.x == 0`, and takes its disabled early-out.

## 4. Mechanism

* `OpenGLUniformBuffer`'s constructor is the **only** place the persistent scene-
  effect UBOs (Fog @17, FogVolumes @20, …) call `glBindBufferBase`. Per-frame uploads
  use `SetData` (`glNamedBufferSubData`), which does **not** touch the binding.
* `OpenGLUniformBuffer`'s destructor does a deferred `glDeleteBuffers`. **Deleting a
  buffer that GL currently has bound at a slot reverts that slot to 0** (GL spec).
* Over a long-lived process (the full suite is ~3,700 tests in one GL context) the
  constant create/destroy churn of *other* buffers is enough to knock binding 17 to
  0. Once knocked out, nothing re-binds it — the Fog upload only refreshes contents,
  and the standalone `FogRenderPass` owns neither the Fog nor FogVolumes UBO.
* Binding **8** never broke because `RenderPipeline.cpp` already re-binds the motion-
  blur UBO after every upload, with a comment warning about *exactly* this hazard.
  Fog/FogVolumes were missing the same guard.

## 5. Fix

`OloEngine/src/OloEngine/Renderer/RenderPipeline.cpp`, in the per-frame scene-effects
upload — after the `Fog->SetData(...)` (both enabled/disabled branches) and the
`FogVolumes->SetData(...)`:

```cpp
data.SceneEffectsGPU.Fog->Bind();        // re-establish binding 17 every upload
...
data.SceneEffectsGPU.FogVolumes->Bind(); // re-establish binding 20 every upload
```

Mirrors the binding-8 motion-blur re-bind a few lines below. Re-binding at the
per-frame upload site is sufficient: nothing within a frame touches binding 17/20
after the upload, so the slot is correct by the time the late fog pass executes.

## 6. Regression guard

`FogVisualEvidenceTest.ReappliesFogAfterFogUboBindingKnockedOut` reproduces the
corrupt state **deterministically** (no reliance on suite order): it
`glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FOG, 0)` right before the frame, renders
fog ON, then asserts (a) binding 17 is non-zero after the frame and (b) the distant
band is still blue-dominant with a gradient. Without the re-bind it fails exactly
like the suite flake — even in isolation. (L8, SKIPs without a GL 4.6 context.)

## 7. Debugging heuristic (for the next time this class resurfaces)

* **Symptom shape:** a renderer visual test passes alone but the effect is
  under-applied / byte-identical with "off" **only in the full suite** ⇒ suspect
  **cross-test GL state**, not the effect's math (the math contracts pass).
* **First read the binding *ids*, not the contents.** At the consuming pass,
  `glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, slot, &id)` — `id == 0` means the slot
  was knocked out. A "same buffer id at the slot" check that only *assumes* the
  binding is meaningless; query the live binding.
* **Audit every persistent UBO a late/standalone pass reads** for the constructor-
  only-bind pattern. If the pass doesn't own/upload the UBO, the owner must re-bind
  it on upload. Current at-risk slots read by post-process: Camera 0, FogHistory 3,
  Shadow 6, PostProcess 7, MotionBlur 8 (guarded), Fog 17 (now guarded),
  FogVolumes 20 (now guarded).
* **Don't trust a "stale camera position" theory for near-clear + far-underfogged.**
  It is geometrically impossible (a displaced camera also fogs the near band).

## 8. Second finding — binding-0 out-of-bounds camera read (also fixed)

The same read-back exposed a separate, latent bug: during the fog pass **binding 0
holds a 64-byte `ViewProjection`-only camera UBO** (confirmed: `GL_BUFFER_SIZE ==
64`, carrying the scene VP — `vpRow0 == projection[0][0]`), not the full 272-byte
`CameraUBO`. Both fog shaders read past it: `u_CameraPosition` (std140 offset 192,
`PostProcess_Fog.glsl`) and `u_Projection` (offset 128, `PostProcess_FogUpsample
.glsl`). It is **not** the #446 differentiator — the 64-byte buffer is bound in
*both* the passing and failing runs — and origin-centred scenes survive only
because NVIDIA's robust-buffer-access returns **0** for OOB reads, and `cameraPos
≈ (0,0,0) ≈` the true camera there. OOB UBO reads are *undefined* per the GL spec,
and any scene whose camera sits far from the world origin fogs wrong.

**Cause:** several subsystems create their own camera UBO at binding 0 with
different sizes (`Renderer2D` and `ParticleBatchRenderer` use a 64-byte
`mat4 ViewProjection`-only layout; ShadowMap/IBL/sky/the shared UBO use the full
272 bytes). The post-process passes that read binding 0 inherit whatever the last
stage left bound, and no one guarantees the full camera UBO is bound before the
chain runs.

**Fix:** `FogRenderPass::Execute` now re-binds the full shared camera UBO
(`data.SharedSceneUBOs.Camera`, plumbed via `SetCameraUBO`) at slot 0 before
drawing — exactly like its existing binding-7 PostProcess UBO re-bind. Re-binding
at the *upload* site (à la binding 17) would **not** work here, because a later
scene stage re-binds the 64-byte buffer *after* that point; the re-bind has to be
inside the pass.

**Regression guard:** `FogVisualEvidenceOffOriginTest.NearStaysClearWhenSceneIsFar
FromOrigin` builds the identical scene ~1000 units from the origin. Empirically,
without the re-bind the near floor floods blue (`nearOn.R=113 B=182`); with it the
near floor stays warm and only the distance fogs. (L8, SKIPs without a GL context.)
