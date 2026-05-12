# RenderGraph Versioned-Write Seam Adoption

## Status

Linear post-process chain coverage complete (15 production seams). Captured 2026-05-10 as a milestone summary of the producer-owned-seam workstream that ran on `feature/rendergraph_rework`. This document is descriptive, not forward-looking — it records what exists today so future work can build on it without re-deriving the pattern.

## Goal

Make every same-resource rewrite chain in the post-process pipeline an explicit, producer-owned graph contract instead of a canonical-name mutation that depends on registration order. The win is not a new feature — it is removing a class of latent ordering bugs and giving the debugger / frame-capture a stable name for "the version of `Resource` that pass `X` actually wrote."

The primitive is `RGBuilder::WriteNewVersion(handle, usage, passNameTag)`. It clones the source descriptor into a graph-owned, pass-derived resource (`Resource@PassName`), so a downstream consumer can resolve the producer's exact output instead of re-reading whatever the canonical name happens to point at after build.

## The Pattern (recipe)

A "seam" is one producer + N consumers + one builder wiring site:

1. **Producer** — in `Setup()`, replace `builder.Write(blackboard.Resource, ...)` with:

   ```cpp
   constexpr std::string_view versionTag = "ProducerPass";
   const auto outputHandle = builder.WriteNewVersion(blackboard.Resource, usage, versionTag);
   if (!outputHandle.IsValid()) return;
   SetPrimaryOutputFramebufferHandle(outputHandle);
   SetPrimaryOutputTextureHandle(
       builder.CreateFramebufferAttachmentView(
           std::string(ResourceNames::ResourceTexture) + "@" + std::string(versionTag),
           outputHandle, 0u));
   ```

2. **Consumer** — add `void SetPreferredXxxSource(RenderPass*)` plus `RenderPass* m_PreferredXxxSource = nullptr` in the header. In `Setup()`, before the canonical fallback chain, build the preferred input:

   ```cpp
   auto xxxInput = MakeFramebufferTextureInput(blackboard.XxxColor, blackboard.XxxColorTexture);
   if (m_PreferredXxxSource) {
       if (!m_PreferredXxxSource->GetName().empty())
           builder.DependsOnPass(m_PreferredXxxSource->GetName());
       const auto fb = m_PreferredXxxSource->GetPrimaryOutputFramebufferHandle();
       const auto tx = m_PreferredXxxSource->GetPrimaryOutputTextureHandle();
       if (fb.IsValid() && tx.IsValid())
           xxxInput = MakeFramebufferTextureInput(fb, tx);
   }
   // …then xxxInput is one of the candidates passed to ReadFirstValidFramebufferTextureInputForPass(...).
   ```

3. **Builder wiring** — in `RenderPipelineBuilderPost::RegisterPostProcessNodes(...)`, after the consumer's preferred-source-block opens, call `inputs.Passes->Consumer->SetPreferredXxxSource(inputs.Passes->Producer)`.

4. **Focused test** — in `OloEngine/tests/Rendering/RenderGraphTest.cpp`, a single `TEST(RenderGraph, XxxPublishesVersionedOutput…)` that imports the necessary framebuffers, enables the producer, disables the intermediate seams the consumer would otherwise prefer, builds the graph, and asserts each consumer's `GetPrimaryInputFramebufferHandle() == producer->GetPrimaryOutputFramebufferHandle()` plus `graph.GetResourceName(handle) == "Xxx@ProducerPass"`.

## Coverage

The 15 production seams cover the full linear post-process chain:

```
SSS → AOApply → Bloom → DOF → MotionBlur → TAA → Precipitation → Fog →
ChromaticAberration → ColorGrading → ToneMap → Vignette → FXAA → SelectionOutline → UIComposite → Final
```

| Producer | Versioned output | Consumers (in declared preference order) |
|---|---|---|
| `SSSRenderPass` | `SSSColor@SSSPass` | `AOApplyRenderPass`, `BloomRenderPass` |
| `AOApplyRenderPass` | `AOApplyColor@AOApplyPass` | `BloomRenderPass` |
| `BloomRenderPass` | `BloomColor@BloomPass` | `DOFRenderPass`, `MotionBlurRenderPass`, `TAARenderPass`, `PrecipitationRenderPass`, `FogRenderPass`, `ChromaticAberrationRenderPass`, `ColorGradingRenderPass`, `ToneMapRenderPass`, `VignetteRenderPass`, `FXAARenderPass`, `SelectionOutlineRenderPass`, `UICompositeRenderPass` |
| `DOFRenderPass` | `DOFColor@DOFPass` | `MotionBlurRenderPass` and all later post stages |
| `MotionBlurRenderPass` | `MotionBlurColor@MotionBlurPass` | `TAARenderPass` and all later post stages |
| `TAARenderPass` | `TAAColor@TAAPass` | `PrecipitationRenderPass` and all later post stages |
| `PrecipitationRenderPass` | `PrecipitationColor@PrecipitationPass` | `FogRenderPass` and all later post stages |
| `FogRenderPass` | `FogColor@FogPass` | `ChromaticAberrationRenderPass` and all later post stages |
| `ChromaticAberrationRenderPass` | `ChromAbColor@ChromAberrationPass` | `ColorGradingRenderPass` and all later post stages |
| `ColorGradingRenderPass` | `ColorGradingColor@ColorGradingPass` | `ToneMapRenderPass` and all later post stages |
| `ToneMapRenderPass` | `ToneMapColor@ToneMapPass` | `VignetteRenderPass` and all later post stages |
| `VignetteRenderPass` | `VignetteColor@VignettePass` | `FXAARenderPass`, `SelectionOutlineRenderPass`, `UICompositeRenderPass` |
| `FXAARenderPass` | `FXAAColor@FXAAPass` | `SelectionOutlineRenderPass`, `UICompositeRenderPass` |
| `SelectionOutlineRenderPass` | `SelectionOutlineColor@SelectionOutlinePass` | `UICompositeRenderPass` |
| `UICompositeRenderPass` | `UIComposite@UICompositePass` | `FinalRenderPass` |

Canonical base-name handle lookup (`graph.GetFramebufferHandle("XxxColor")`) follows the latest explicit version, so debug, frame-capture, and external-name resolution paths stay aligned with the producer-owned outputs without their own knowledge of the version tag.

Each seam has a focused gtest in `OloEngine/tests/Rendering/RenderGraphTest.cpp` named `RenderGraph.<Producer>PublishesVersionedOutput…`. They run cheaply (single-digit ms each) because they only exercise builder wiring, not GPU work — no shaders are compiled and `m_Enabled = false` on intermediate seams forces the fallback chain to be exercised.

## What's intentionally not covered

The seam pattern is shaped for one-producer, multi-consumer, one-output linear chains where consumers read a sampled framebuffer attachment. These call sites in `OloEngine/src/OloEngine/Renderer/Passes` are real outputs but a different shape, and the existing primitive doesn't cleanly apply:

- **Read-modify-write feedback chains** — `SceneColor` is rewritten by `ForwardOverlayRenderPass`, `FoliageRenderPass`, `DecalRenderPass`, `WaterRenderPass`, `ParticleRenderPass`, `OITResolveRenderPass` after `DeferredLightingPass`/`SceneRenderPass` writes it; `OITAccum` and `OITRevealage` are cleared by `OITPrepareRenderPass` and then RMW'd by `DecalRenderPass` and `ParticleRenderPass` in their OIT paths. Versioning would conflict with the explicit `AllowFeedback(...)` declarations these passes already make. The chain ordering for both families is now pinned (2026-05-10) by explicit `DependsOnPass(previous_writer)` edges — see `docs/rendergraph-ue-gap-analysis-and-refactor-plan.md` "Topology Robustness Audit" → Phase B. The mechanism is different from the producer-owned-seam pattern (no aliasing version handle, just an explicit ordering edge), but the correctness outcome is the same: registration order is no longer load-bearing for these RMW chains.
- **Sampled-texture outputs** — `AOBuffer` (written by `SSAORenderPass` / `GTAORenderPass`, read by `AOApplyRenderPass`) is consumed via `builder.Read(handle, ShaderSample)`, not through the framebuffer-and-texture-handle pair the preferred-source helpers operate on. The dependency is already explicit; no seam would change anything.
- **Array-layer / cube-face producers** — `ShadowMapCSM`, `ShadowMapSpot` are written one layer at a time and consumed via array/cube views; versioning would need to cover view families, not parent handles.
- **Intra-pass scratch with feedback** — `BloomMips`, `FogHalfRes`, `JFAPing`/`JFAPong`, `GTAOEdge`/`GTAODenoisePing`/`GTAODenoisePong`, `HZBDepth` mip pyramid, `SSAORaw`/`SSAOBlur`. Already declared as feedback inside one node.
- **Multi-target G-buffer write-back** — `DeferredOpaqueDecalPass` blends decals into eight G-buffer attachments as `TransferDest`. Multi-output, no single producer-owned handle to publish.
- **Terminal** — `FinalRenderPass` writes `Backbuffer`. Nothing reads from it.

## When to add a new seam

A new producer-owned seam is appropriate when **all** of these hold:

1. The producer writes a single primary framebuffer that downstream passes sample as input (not feedback, not array-layer).
2. There is more than one downstream consumer, OR the producer is part of an opt-in chain where a downstream consumer would otherwise depend on registration order to bind to the right canonical version.
3. The chain's intermediate stages can be disabled at runtime, and the absence of a stage should fall through to the next-most-recent producer (this is what makes the preferred-source chain useful versus a simple `DependsOnPass`).

If only (1) holds, the existing canonical `builder.Write(...)` is fine — no consumer needs to disambiguate. If (2) holds but (3) does not (e.g. a fixed always-on producer with one always-on consumer), `DependsOnPass(ProducerName)` plus a canonical write is enough.

## Where it lives in code

- Primitive: `RGBuilder::WriteNewVersion(...)`, `RGBuilder::CreateFramebufferAttachmentView(...)` in `OloEngine/src/OloEngine/Renderer/RGBuilder.cpp`.
- Producer/consumer pattern: any of the 15 passes in `OloEngine/src/OloEngine/Renderer/Passes/` (see table above for filenames).
- Builder wiring: `OloEngine/src/OloEngine/Renderer/RenderPipelineBuilderPost.cpp` — single `RegisterPostProcessNodes(...)` function holds the entire seam ladder.
- Canonical-name resolution: `RenderGraph::GetFramebufferHandle(...)` and `GetTextureHandle(...)` in `OloEngine/src/OloEngine/Renderer/RenderGraph.cpp` follow the latest explicit version automatically.
- Tests: `OloEngine/tests/Rendering/RenderGraphTest.cpp`, all named `RenderGraph.<Producer>PublishesVersionedOutput…`.

## Related docs

- `docs/rendergraph-modernization-plan.md` — broader modernization context and the running list of completed slices.
- `docs/rendergraph-ue-gap-analysis-and-refactor-plan.md` — gap analysis vs. UE-style render graphs; "Resource versioning" row tracks the same milestone.
