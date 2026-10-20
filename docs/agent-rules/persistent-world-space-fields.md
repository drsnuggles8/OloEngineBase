# A persistent world-space field that never fades, and every math test green

Rules for any accumulation buffer the engine keeps across frames and that decays: the water disturbance field, the snow-depth clipmap, a damage map, a decal fade buffer, a footprint field. Read before touching one.
Applies to: `OloEngine/src/OloEngine/Renderer/Water/WaterDisturbanceField.h`, `WaterDisturbanceSystem.cpp`, `OloEditor/assets/shaders/compute/WaterDisturbance_Update.comp`, `include/WaterDisturbanceCommon.glsl`.

## Rules

1. **Choose the storage format from the decay rate, not from the value range.** With `value *= factor` per frame, an 8-bit normalized texture rounds to 1/255, so decay only happens if `v * (1 - factor)` exceeds `0.5 / 255`; below that the field never changes again. Use a 16-bit float. [Details](persistent-world-space-fields-details.md#1-a-multiplicative-decay-is-unrepresentable-in-a-normalized-integer-texture-below-a-rate-threshold)
2. **Test the quantisation, not only the decay formula.** Run the recurrence through both quantisers for 600 frames, as `WaterDisturbanceFieldTest.DecayStallsUnderR8QuantisationButNotUnder16BitFloat` does, and `ASSERT_LT(perFrameStep, 0.5f/255.0f)` first so the test cannot silently assert nothing. Assert "barely changed", not "exactly where it started". [Details](persistent-world-space-fields-details.md#1-a-multiplicative-decay-is-unrepresentable-in-a-normalized-integer-texture-below-a-rate-threshold)
3. **Use `RG16F` where you need a 16-bit float, not a new single-channel format.** `ImageFormat` has no single-channel 16-bit float; adding one risks the `R32UInt` failure (nothing to map to on Vulkan, silently returns the NULL texture handle). [Details](persistent-world-space-fields-details.md#1-a-multiplicative-decay-is-unrepresentable-in-a-normalized-integer-texture-below-a-rate-threshold)
4. **Take the field's `dt` from `Time::GetTime()` against a stored previous sample, never a `static steady_clock`.** Store it like `Renderer3DData::WaterDisturbancePrevTimeSeconds` (mirrors `CloudPrevTimeSeconds`), so `Time::SetMockTime` freezes the field and goldens are stable. [Details](persistent-world-space-fields-details.md#3-a-persistent-fields-dt-must-come-from-the-mockable-clock-or-none-of-its-goldens-are-stable)
5. **Tick the field in tests; do not fast-forward the clock.** `dt` is clamped (0.25 s here), so one frame after a 12 s jump decays 0.25 s. Advance in steps under the clamp, one render each (`DecayFor`). [Details](persistent-world-space-fields-details.md#3-a-persistent-fields-dt-must-come-from-the-mockable-clock-or-none-of-its-goldens-are-stable)
6. **Do not rely on an RMSE golden for placement or fade.** Mirrored wake, half-texel offset, a phantom second wake after a window shift and R8 non-fade all look plausible. Assert from the readback: a trail exists, it is in the right place (centre column vs two symmetric off-trail columns in the same frame), and it fades. [Details](persistent-world-space-fields-details.md#4-what-the-golden-cannot-tell-you-and-what-to-assert-instead)
7. **Measure fade as contrast (trail minus nearby open water, same frame), not brightness.** Advancing the clock also advances the wave phase; the absolute frame came out brighter after decay. The test requires contrast to fall below 60% of the starting value. [Details](persistent-world-space-fields-details.md#3-must-be-measured-as-contrast-not-brightness--the-clock-moves-the-sea-too)
8. **Take sampling bands from a real capture, not from percentages.** Capture, read the PNG, take a row/column profile, write the measured pixel bounds and values into the test comment. [Details](persistent-world-space-fields-details.md#measure-the-sampling-bands-off-a-real-capture-do-not-derive-them-from-fractions)
9. **Add the `IsKnownUBOBinding` / `IsKnownTextureBinding` case in the same commit as a new binding constant.** The cases in `ShaderBindingLayout` match block/sampler name as well as number; a missing case fails `ShaderReflectionBinding.AllProductionShaderBindingsMatchCppLayout`, which runs only in the full ctest sweep. Run `--gtest_filter='Shader*'` before pushing. [Details](persistent-world-space-fields-details.md#4b-a-new-binding-constant-is-not-a-new-binding--the-validator-has-to-learn-it)
10. **Treat a new `TEX_*` slot as a shader edit.** `HEAP_IMAGE_SLOT_BASE` derives from `TEX_SHADER_GRAPH_0`, but the GLSL `OLO_HEAP_IMAGE_BASE` is a hand-written literal (and so is the copy in `BindlessHeapGpuTest.cpp`). If `BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout` fails, fix the literal, not the test. [Details](persistent-world-space-fields-details.md#5-adding-a-tex_-slot-is-also-a-shader-edit--and-it-moves-a-hand-written-literal)
11. **Reset the field on scene entry and publish the disabled state unconditionally.** `Scene::OnRuntimeStart` / `OnSimulationStart` call `Reset()` and drop the pose history (its timestamps are against `m_SimulationTime`, which they zero). `GetShaderParams()` returns intensity 0 whenever the field is unusable, and the scene publishes settings even when disabled. [Details](persistent-world-space-fields-details.md#6-where-this-kind-of-field-belongs-and-where-it-does-not)
12. **Keep the field out of the render graph.** `WriteNewVersion` renames resources and the transient pool aliases memory, so a texture that must survive a frame is a stale-pool-read. Use a static singleton dispatched from `RenderPipeline`, like `SnowAccumulationSystem`. [Details](persistent-world-space-fields-details.md#6-where-this-kind-of-field-belongs-and-where-it-does-not)
13. **Draw a diverging shape from the lookup, not the stamp.** A per-frame stamp cannot make a V wake; keep a bounded time-stamped pose history and lay the arms at the pose that is `kArmAgeSeconds` old, offset by a function of that age. Time-extent goes in the read. [Details](persistent-world-space-fields-details.md#2-a-per-frame-stamp-cannot-draw-a-diverging-shape--the-divergence-has-to-come-from-the-lookup)
14. **Validate once at the submission boundary and keep CPU/GPU mirrored math identical.** `WaterDisturbance::SplatWeight` and `waterDisturbanceSplatWeight` match expression for expression with `max(x, 1e-4)` guards; non-finite inputs are rejected in `WaterDisturbanceSystem::SubmitSplat`. [Details](persistent-world-space-fields-details.md#7-one-validation-boundary-so-the-cpu-and-gpu-halves-can-stay-literal-mirrors)

## Commands and checks

```
--gtest_filter='Shader*'
```

Run before pushing a new shader binding.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[persistent-world-space-fields-details.md](persistent-world-space-fields-details.md).
