# Subsystem notes — renderer, GL layer, shaders, offline capture

Gotchas for render passes, GL wrappers, SSAO/SSR/FSR, IBL and offline capture. Read before editing renderer code.

## Rules

1. **Read offline captures from the graph's own targets.** Use `Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor)`, render a warmup frame after resize, pass `IBLConfiguration{ .UseDiskCache = false }`, force render scale 1.0 via a scope guard. [Details](notes-renderer-details.md#1-offline-capture-via-scenerenderscene3d--five-silently-black-traps)
2. **Drain `glGetError()` before checking it after a readback.** **When reassigning `m_RendererID`, keep the old handle until the new one is valid,** then `SubmitForDeletion` it, balance the GPU tracker and unregister side maps. [Details](notes-renderer-details.md#2-drain-glgeterror-before-checking-it-after-a-readback)
3. **Reset raw GL ids published into `Renderer3D` globals every frame in `RenderPipeline::PrepareFrame`.** [Details](notes-renderer-details.md#4-a-raw-gl-id-published-into-global-renderer-state-must-be-reset-every-frame)
4. **A new shader binding needs the constant plus `IsKnownUBOBinding` / `IsKnownTextureBinding` cases.** [Details](notes-renderer-details.md#5-adding-a-shader-binding-is-three-edits-not-one)
5. **There are no geometry shaders; do not assume layered rendering.** **Do not add a parallel IBL bake knob.** [Details](notes-renderer-details.md#6-there-is-no-geometry-shader-stage)
6. **A pass writing scene colour or the G-Buffer after `SceneRenderPass` declares the framebuffer write;** never copy an attachment into an export. Register writers before readers; same-framebuffer sampling reads `Scene.SceneDepthSnapshot`. `OLO_RG_COPY_LEDGER=1` counts copies. [Details](notes-renderer-details.md#8-adding-a-pass-that-writes-the-scene-target-or-the-g-buffer-after-scenerenderpass)
7. **Call `Renderer3D::ApplyRendererSettings()` after changing settings;** push live state in BOTH scene-load finalizers (`LoadEditorSceneFile`, `LoadSceneInternal`). [Details](notes-renderer-details.md#9-renderersettings-only-reach-the-live-graph-through-applyrenderersettings)
8. **Headless scene loads must copy the scene's settings blocks into `Renderer3D` themselves,** then restore the globals. [Details](notes-renderer-details.md#a-scenes-serialized-settings-blocks-do-not-reach-the-renderer-through-sceneserializerdeserialize-alone)
9. **Gate shadow work on `ShadowMap::AnyShadowsRequested()`.** **Assert AO on the AO buffer via `SSAODebugView`, not the lit frame.** [Details](notes-renderer-details.md#10-shadow-pass-stale-csm-matrices-dont-self-skip)
10. **SSR keeps its own min-depth HZB and must reproduce v1's reflection.** [Details](notes-renderer-details.md#12-ssr-owns-its-own-min-depth-hzb)
11. **Upscaler declaration key must carry `data.PostProcess.Upscale`;** EASU must not list `PostProcessColor`. [Details](notes-renderer-details.md#13-fsr1-three-port-bugs-and-the-reduced-size-target-wiring)
12. **FSR2 policy lives in `TemporalUpscalePolicy.h` (`FSR2PolicyTest`).** Key the declaration on `data.TemporalUpscaleActive`, force TAA and late RCAS off while it runs. [Details](notes-renderer-details.md#13b-fsr2-temporal-upscaling-684--what-is-different-from-fsr1-and-where-it-hides)
13. **Wrap a third-party GPU dispatch that binds by fixed slot in capture/restore of only the slots it reaches**. **Measure restore cost after other tests have run.** [Details](notes-renderer-details.md#a-third-party-gpu-dispatch-owns-the-binding-namespace-it-writes-to--fsr2s-36-darkening)
14. **Keep private-array indices literal in GLSL for NVIDIA's GL SPIR-V path.** [Details](notes-renderer-details.md#keep-private-array-indices-literal-in-glsl-headed-for-nvidias-gl-spir-v-path--that-was-fsr2s-3x)
15. **Frame/pass GPU timers use `GL_TIMESTAMP` pairs, never nested `GL_TIME_ELAPSED`; read results 1-3 frames late.** **Precision tests use exactly-f32 coordinates; `Renderer2D::SetLineWidth(1.0f)` before lines.** [Details](notes-renderer-details.md#14-gpu-timer-queries)
16. **Diagnose probabilistic driver crashes with env-gated A/B runs across relaunches.** [Details](notes-renderer-details.md#16-a-probabilistic-driver-crash-needs-ab-experiments-not-the-stack)
17. **A stage after `UICompositePass` must reach all four presented-image consumers:** `FinalRenderPass::Setup`, `EditorLayer` viewport, MCP capture, `RenderGraphFrameCapture.cpp`. [Details](notes-renderer-details.md#17-a-post-stage-after-uicompositepass-has-four-consumers-and-three-of-them-are-not-the-graph)
18. **Never read the write-only RHI persistent mapping on the CPU;** keep a host-authoritative copy. [Details](notes-renderer-details.md#18-the-rhis-persistent-mapping-is-write-only--a-gpu-visible-cpu-structure-needs-host-authority-not-mapped-reads)
19. **A/B shader changes live by neutralising them narrowly, restoring from a copy, with an unaffected control pose.** [Details](notes-renderer-details.md#19-ab-a-shader-side-renderer-change-against-the-live-editor-without-rebuilding)
20. **Author a COMPLETE `PrecipitationSettings` block for its `Type`;** struct defaults are snow. [Details](notes-renderer-details.md#20-a-scenes-precipitationsettings-block-must-be-complete-because-the-struct-defaults-are-snow)
21. **Budget fog density against `cameraFar`; fog reaches the sky.** [Details](notes-renderer-details.md#21-distance-fog-reaches-the-sky-so-a-clear-day-haze-veils-the-whole-frame)
22. **A pass needing every occluder uses the DDGI caster funnel** (`AddDDGICaster`), gathered in `ConfigurePassesForFrame`, not `Setup()`. [Details](notes-renderer-details.md#a-pass-that-needs-every-occluder-on-screen-or-not-must-take-the-ddgi-caster-funnel)
23. **Test accumulating terms with many occluders; window a hard-culled contribution to zero at the cull radius.** [Details](notes-renderer-details.md#a-one-occluder-test-scene-cannot-see-an-accumulation-bug)
24. **Call `Bind()` before every use when two uniform/storage buffers share a binding point;** add `Renderer3D::Init` buffers to `RebindPersistentBuffers`. [Details](notes-renderer-details.md#a-uniformbuffer-claims-its-binding-point-at-construction-so-two-of-them-on-one-binding-is-last-created-wins)
25. **When tightening a loop bound, re-check every guard calibrated against the old one.** [Details](notes-renderer-details.md#tightening-a-loop-bound-moves-every-guard-that-was-calibrated-against-it)
26. **Draw a new 2D component at all four `Scene.cpp` sites**. [Details](notes-renderer-details.md#adding-a-world-space-2d-component-means-editing-four-draw-sites-and-they-do-not-agree-today)
27. **Hand-built textures are vertically mirrored versus loaded ones;** emit bottom row first, assert tile identity. [Details](notes-renderer-details.md#a-texture-built-in-memory-is-vertically-mirrored-relative-to-one-loaded-from-a-file)
28. **Composite competing estimates bottom-up with `mix()`;** a new tier's enable flag must reach the declaration key. [Details](notes-renderer-details.md#an-ordered-hierarchy-of-light-sources-must-composite-bottom-up-or-every-tier-needs-data-it-cannot-reach)
29. **Key regenerated-instance identity on the generator's input slot,** ids from a monotonic counter. [Details](notes-renderer-details.md#a-regenerate-wholesale-system-must-key-identity-on-the-generators-input-slot-not-its-output-row)
30. **Read terrain with `GetHeightData()` and `TerrainData::SampleHeight`, not per-cell `GetHeightAt`.** **Give any system derived from the height field a rebuild flag beside the existing three in `Scene.cpp`.** [Details](notes-renderer-details.md#terraindatas-height-queries-each-sync-from-the-gpu--do-not-call-them-per-cell)
31. **Insert a `PBRMaterialUBO` field before `u_MaterialHeapOffsets` in all eight GLSL blocks** (`grep -rl PBRMaterialProperties OloEditor/assets/shaders`). [Details](notes-renderer-details.md#a-material-field-added-to-pbrmaterialubo-must-be-declared-in-all-eight-glsl-blocks)
32. **New RT2 flags fields go below the open-ended closure model and are clamped on encode;** keep the skin and foliage slot constants separate. [Details](notes-renderer-details.md#a-g-buffer-flags-lane-with-an-open-ended-field-cannot-absorb-a-new-one-for-free)
33. **RT5 `.rgb` may hold a kind-gated scalar only if the read is gated on your writer and the lightmap wins.** [Details](notes-renderer-details.md#g-buffer-rt5-is-a-free-per-pixel-lane-for-a-kind-gated-scalar-because-its-alpha-is-a-gate)
34. **Bias two-sided shadow lookups along the lit-side normal.** [Details](notes-renderer-details.md#a-two-sided-surfaces-shadow-lookup-must-be-biased-along-the-lit-side-normal)
35. **Check `Texture2D::IsLoaded()` then drop the `Ref`;** `Create` never returns null. [Details](notes-renderer-details.md#texture2dcreatepath--never-returns-null--isloaded-is-the-only-thing-that-says-so)
36. **A new `GPUSceneUnsupportedCategory` also needs `UnsupportedCategoryKey` in `McpRayTracingStats.h`.** [Details](notes-renderer-details.md#adding-a-gpusceneunsupportedcategory-is-four-edits-and-using-enum-hides-the-fourth)
37. **Build the control that removes an error before tuning its constant.** **Write guards against what ships (iterate the enum);** keep version lists as one named predicate. [Details](notes-renderer-details.md#measure-which-error-term-dominates-before-repairing-either-of-the-others)
38. **Publish every storage buffer a shader declares, every frame it draws.** **Turn in-place post passes off under material debug views.** [Details](notes-renderer-details.md#a-buffer-a-shader-declares-must-be-published-every-frame-even-when-the-shader-gates-its-reads)
39. **Re-run a failing sweep test alone; never `git add -A` after a sweep.** [Details](notes-renderer-details.md#a-full-local-ctest-sweep-is-1841-tests-and-runs-for-hours)

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[notes-renderer-details.md](notes-renderer-details.md).
