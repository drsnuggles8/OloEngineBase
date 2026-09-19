#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"

#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <vector>

// =============================================================================
// Render-graph blackboard-fingerprint sensitivity tests.
//
// WHY THIS EXISTS
// ---------------
// PopulateBlackboard (which declares the per-frame transient render-graph
// resources) short-circuits when ComputeBlackboardFingerprint() matches the
// previous frame. So ANY setting that changes which resources get declared —
// every post-process effect's enable flag, the AO technique, the render path,
// MSAA, OIT, etc. — MUST be folded into that fingerprint. If one is missing,
// toggling it at runtime does NOT re-populate the blackboard: the effect's
// resource is never declared, its pass is culled, and the effect silently
// no-ops. That is exactly the bug the SSR pass shipped with (SSREnabled was
// absent from the fingerprint), and it was invisible to every existing test
// because nothing asserted the fingerprint reacts to settings.
//
// These tests pin the invariant for the WHOLE CLASS of bug: each toggle that
// gates a graph resource must change the fingerprint. They are pure CPU (no GL
// context) — a default Renderer3DData builds its own RenderPipeline, and
// ComputeBlackboardFingerprint only reads settings + (null) scene dims — so
// they run in headless CI, which is precisely where the SSR bug slipped
// through.
//
// MAINTENANCE: when you add a new post-process effect that conditionally
// declares a graph resource in PopulateBlackboard, add its enable flag both to
// ComputeBlackboardFingerprint (RenderPipeline.cpp) AND to kTopologyToggles
// below. A new toggle missing from the fingerprint makes this test fail.
//
// Classification: plumbing (render-graph cache-invalidation contract, no GL).
// =============================================================================

namespace OloEngine::Tests
{
    // Friended by Renderer3D so the test can reach the private RenderPipeline /
    // Renderer3DData and call the otherwise-internal fingerprint hook.
    struct RenderPipelineFingerprintAccess
    {
        [[nodiscard]] static u64 Fingerprint(const PostProcessSettings& pp, const RendererSettings& rs)
        {
            Renderer3D::Renderer3DData data; // ctor makes its own RenderPipeline; no GL
            data.PostProcess = pp;
            data.Settings = rs;
            return data.Pipeline->ComputeBlackboardFingerprint(data);
        }

        // The IDENTITIES the blackboard imports (as opposed to declares). They are
        // not settings, so they need their own hook.
        //
        // Handles, not raw GL ids, since issue #691 — and the
        // fingerprint hashes them for a reason this test now covers by
        // construction: ShadowMap/DDGI free their textures BEFORE recreating, so
        // GL may reissue the same name and a raw-id hash would see no change.
        struct ImportedIBL
        {
            RHI::ResourceHandle Irradiance{};
            RHI::ResourceHandle Prefilter{};
            RHI::ResourceHandle BRDFLut{};
            RHI::ResourceHandle Environment{};
        };

        [[nodiscard]] static u64 FingerprintWithIBL(const ImportedIBL& ibl)
        {
            Renderer3D::Renderer3DData data;
            data.Settings = RendererSettings{};
            data.Settings.Path = RenderingPath::Deferred;
            data.GlobalIrradianceMapID = ibl.Irradiance;
            data.GlobalPrefilterMapID = ibl.Prefilter;
            data.GlobalBRDFLutMapID = ibl.BRDFLut;
            data.GlobalEnvironmentMapID = ibl.Environment;
            return data.Pipeline->ComputeBlackboardFingerprint(data);
        }

        // Fingerprint with one render-stream pass attached, optionally holding a
        // draw. `select` picks which member of the pass set to build, so the
        // caller names a pass rather than reaching into the private set itself.
        enum class Stream
        {
            Foliage,
            Decal,
            ForwardOverlay,
            Water,
        };

        struct StreamFingerprints
        {
            u64 Empty = 0;    // the bucket has no draws
            u64 WithDraw = 0; // the same pass, one draw submitted
            u64 Restated = 0; // recomputed with no further change
        };

        // Both fingerprints come from ONE Renderer3DData holding ONE pass
        // instance, with a draw submitted in between. That is deliberate:
        // HashPassState folds in the pass POINTER, so building a second
        // pipeline to represent "the other frame" would differ for a reason
        // that has nothing to do with the bucket. It is also what actually
        // happens — the pass outlives the frame and its bucket fills.
        [[nodiscard]] static StreamFingerprints FingerprintAcrossFirstDraw(Stream stream)
        {
            Renderer3D::Renderer3DData data;
            data.Settings = RendererSettings{};
            data.Settings.Path = RenderingPath::Deferred;

            auto& passes = data.Pipeline->RenderStreamPasses;
            CommandBufferRenderPass* node = nullptr;
            switch (stream)
            {
                case Stream::Foliage:
                    passes.Foliage = Ref<FoliageRenderPass>::Create();
                    node = passes.Foliage.Raw();
                    break;
                case Stream::Decal:
                    passes.Decal = Ref<DecalRenderPass>::Create();
                    node = passes.Decal.Raw();
                    break;
                case Stream::ForwardOverlay:
                    passes.ForwardOverlay = Ref<ForwardOverlayRenderPass>::Create();
                    node = passes.ForwardOverlay.Raw();
                    break;
                case Stream::Water:
                    passes.Water = Ref<WaterRenderPass>::Create();
                    node = passes.Water.Raw();
                    break;
            }

            StreamFingerprints out;
            out.Empty = data.Pipeline->ComputeBlackboardFingerprint(data);

            // The allocator outlives both remaining calls; the packet's
            // contents are never read, only counted.
            CommandAllocator allocator;
            ClearCommand payload{};
            node->GetCommandBucket().Submit(payload, {}, &allocator);

            out.WithDraw = data.Pipeline->ComputeBlackboardFingerprint(data);
            out.Restated = data.Pipeline->ComputeBlackboardFingerprint(data);
            return out;
        }
    };

    namespace
    {
        [[nodiscard]] RendererSettings DeferredSettings()
        {
            RendererSettings rs;
            // Deferred so deferred-only effects (e.g. SSR) are eligible; the
            // fingerprint must react to their toggles regardless, but this keeps
            // the scenario realistic.
            rs.Path = RenderingPath::Deferred;
            return rs;
        }

        struct Toggle
        {
            const char* Name;
            void (*Apply)(PostProcessSettings&);
        };

        // Every post-process flag that gates a graph-resource declaration /
        // pass execution. Keep in sync with ComputeBlackboardFingerprint.
        const std::vector<Toggle> kTopologyToggles = {
            { "SSAOEnabled", [](PostProcessSettings& s)
              { s.SSAOEnabled = !s.SSAOEnabled; } },
            { "GTAOEnabled", [](PostProcessSettings& s)
              { s.GTAOEnabled = !s.GTAOEnabled; } },
            { "SSREnabled", [](PostProcessSettings& s)
              { s.SSREnabled = !s.SSREnabled; } },
            { "SSGIEnabled", [](PostProcessSettings& s)
              { s.SSGIEnabled = !s.SSGIEnabled; } },
            // Issue #708: half resolution sizes every resource in the SSGI
            // denoiser chain AND its four temporal histories, so it is a
            // topology toggle even though it declares no new resource.
            { "SSGIHalfResolution", [](PostProcessSettings& s)
              { s.SSGIHalfResolution = !s.SSGIHalfResolution; } },
            { "BloomEnabled", [](PostProcessSettings& s)
              { s.BloomEnabled = !s.BloomEnabled; } },
            { "DOFEnabled", [](PostProcessSettings& s)
              { s.DOFEnabled = !s.DOFEnabled; } },
            { "MotionBlurEnabled", [](PostProcessSettings& s)
              { s.MotionBlurEnabled = !s.MotionBlurEnabled; } },
            { "TAAEnabled", [](PostProcessSettings& s)
              { s.TAAEnabled = !s.TAAEnabled; } },
            { "CASEnabled", [](PostProcessSettings& s)
              { s.CASEnabled = !s.CASEnabled; } },
            { "ChromaticAberrationEnabled", [](PostProcessSettings& s)
              { s.ChromaticAberrationEnabled = !s.ChromaticAberrationEnabled; } },
            { "ColorGradingEnabled", [](PostProcessSettings& s)
              { s.ColorGradingEnabled = !s.ColorGradingEnabled; } },
            { "VignetteEnabled", [](PostProcessSettings& s)
              { s.VignetteEnabled = !s.VignetteEnabled; } },
            { "FXAAEnabled", [](PostProcessSettings& s)
              { s.FXAAEnabled = !s.FXAAEnabled; } },
        };
    } // namespace

    // The regression that motivated the suite: SSREnabled must change the
    // fingerprint, else SSR never re-populates / runs when toggled at runtime.
    TEST(RenderGraphFingerprint, SSREnableTogglesFingerprint)
    {
        const RendererSettings rs = DeferredSettings();

        PostProcessSettings off;
        off.SSREnabled = false;
        PostProcessSettings on = off;
        on.SSREnabled = true;

        EXPECT_NE(RenderPipelineFingerprintAccess::Fingerprint(off, rs),
                  RenderPipelineFingerprintAccess::Fingerprint(on, rs))
            << "Toggling SSREnabled must change the blackboard fingerprint; otherwise "
               "PopulateBlackboard short-circuits and SSRColor is never declared (the original SSR bug).";
    }

    // The whole class: each topology-affecting toggle must flip the fingerprint.
    TEST(RenderGraphFingerprint, EveryTopologyToggleChangesFingerprint)
    {
        const RendererSettings rs = DeferredSettings();
        const PostProcessSettings base; // all effects default (mostly off)
        const u64 baseFp = RenderPipelineFingerprintAccess::Fingerprint(base, rs);

        for (const Toggle& t : kTopologyToggles)
        {
            PostProcessSettings flipped = base;
            t.Apply(flipped);
            EXPECT_NE(RenderPipelineFingerprintAccess::Fingerprint(flipped, rs), baseFp)
                << "Toggling '" << t.Name
                << "' did not change the blackboard fingerprint. Add it to "
                   "ComputeBlackboardFingerprint() in RenderPipeline.cpp, or the render graph "
                   "won't repopulate when it changes at runtime and the effect will silently no-op.";
        }
    }

    // The AO technique selector and the render path also gate graph topology.
    TEST(RenderGraphFingerprint, AOTechniqueAndRenderPathChangeFingerprint)
    {
        const RendererSettings rs = DeferredSettings();
        PostProcessSettings base;

        base.ActiveAOTechnique = AOTechnique::SSAO;
        PostProcessSettings gtao = base;
        gtao.ActiveAOTechnique = AOTechnique::GTAO;
        EXPECT_NE(RenderPipelineFingerprintAccess::Fingerprint(base, rs),
                  RenderPipelineFingerprintAccess::Fingerprint(gtao, rs))
            << "ActiveAOTechnique must change the fingerprint.";

        RendererSettings forward = rs;
        forward.Path = RenderingPath::Forward;
        EXPECT_NE(RenderPipelineFingerprintAccess::Fingerprint(base, rs),
                  RenderPipelineFingerprintAccess::Fingerprint(base, forward))
            << "RenderingPath must change the fingerprint (deferred vs forward declares different resources).";
    }

    // Colour-vision adaptation (issue #458) is the one topology gate that does
    // NOT live in PostProcessSettings — it is a process-global player preference
    // (see AccessibilitySettings.h for why), so EveryTopologyToggleChangesFingerprint
    // above structurally cannot cover it: that test only varies the settings it is
    // handed. Without its own test the mode would be exactly the SSREnabled bug
    // this file exists for — toggle it at runtime, the blackboard never
    // repopulates, ColorBlindColor is never declared, and the stage silently
    // no-ops.
    TEST(RenderGraphFingerprint, ColorBlindModeTogglesFingerprint)
    {
        const RendererSettings rs = DeferredSettings();
        const PostProcessSettings pp;

        Accessibility::Reset();
        const u64 offFp = RenderPipelineFingerprintAccess::Fingerprint(pp, rs);

        AccessibilitySettings on;
        on.ColorBlind = ColorBlindMode::Deuteranopia;
        Accessibility::Set(on);
        const u64 onFp = RenderPipelineFingerprintAccess::Fingerprint(pp, rs);

        // Value-only knobs ride the UBO and must NOT perturb the fingerprint —
        // otherwise dragging the severity slider forces a full graph repopulate
        // every frame.
        AccessibilitySettings tweaked = on;
        tweaked.ColorBlindSeverity = 0.35f;
        tweaked.ColorBlindMethod = ColorBlindAdaptation::Simulate;
        Accessibility::Set(tweaked);
        const u64 tweakedFp = RenderPipelineFingerprintAccess::Fingerprint(pp, rs);

        Accessibility::Reset();

        EXPECT_NE(onFp, offFp)
            << "Enabling a colour-blind mode must change the blackboard fingerprint; "
               "otherwise PopulateBlackboard short-circuits and ColorBlindColor is never declared.";
        EXPECT_EQ(tweakedFp, onFp)
            << "Severity / method / gamma are per-frame UBO uploads, not graph topology — "
               "they must NOT change the fingerprint.";
    }

    // Design-boundary guard: a pure VALUE knob that is uploaded via UBO each
    // frame (and does NOT change which resources are declared) must NOT perturb
    // the fingerprint — otherwise every slider drag would force a full graph
    // repopulate. This documents the intended split between topology-affecting
    // flags (hashed) and per-frame value params (not hashed).
    TEST(RenderGraphFingerprint, ValueOnlyParamDoesNotChangeFingerprint)
    {
        const RendererSettings rs = DeferredSettings();
        PostProcessSettings base;
        base.SSREnabled = true;
        const u64 baseFp = RenderPipelineFingerprintAccess::Fingerprint(base, rs);

        PostProcessSettings tweaked = base;
        tweaked.SSRIntensity = base.SSRIntensity + 1.5f;
        tweaked.SSRMaxDistance = base.SSRMaxDistance + 25.0f;
        tweaked.SSRMaxRoughness = 0.3f;

        EXPECT_EQ(RenderPipelineFingerprintAccess::Fingerprint(tweaked, rs), baseFp)
            << "SSR value params (intensity/distance/roughness) are per-frame UBO uploads, not graph "
               "topology — they must NOT change the blackboard fingerprint.";
    }

    // Same class of bug as SSREnabled, but for an IMPORTED resource rather than a declared
    // one — and this one shipped and fired thousands of times a second.
    //
    // PopulateBlackboard imports the IBL textures into the graph by RAW GL ID. Switching
    // scenes destroys the old EnvironmentMap — deleting those GL textures — and builds new
    // ones with new IDs. Nothing else in the fingerprint changes, so PopulateBlackboard
    // short-circuited and the graph went on resolving the DELETED IDs; DeferredLightingPass
    // bound them every frame and the driver logged
    //   "GL_INVALID_OPERATION ... <texture> is not a valid texture name"
    // once per frame, forever. It looked intermittent only because GL frequently recycles
    // freed texture names, in which case the stale ID silently happens to be valid again.
    //
    // The shadow-map renderer IDs were already hashed for exactly this reason. These are the
    // same thing and were simply missed.
    TEST(RenderGraphFingerprint, ChangingAnImportedIBLTextureIdChangesFingerprint)
    {
        using Access = RenderPipelineFingerprintAccess;
        const Access::ImportedIBL base{ .Irradiance = TestHandle(26u), .Prefilter = TestHandle(45u), .BRDFLut = TestHandle(46u), .Environment = TestHandle(12u) };
        const u64 baseFp = Access::FingerprintWithIBL(base);

        // Each ID must independently invalidate: a scene switch can change any subset.
        {
            Access::ImportedIBL changed = base;
            changed.Irradiance = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalIrradianceMapID is hashed by IDENTITY — a change must repopulate the blackboard.";
        }
        {
            Access::ImportedIBL changed = base;
            changed.Prefilter = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalPrefilterMapID is hashed by IDENTITY — a change must repopulate the blackboard.";
        }
        {
            Access::ImportedIBL changed = base;
            changed.BRDFLut = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalBRDFLutMapID is hashed by IDENTITY — a change must repopulate the blackboard.";
        }
        {
            Access::ImportedIBL changed = base;
            changed.Environment = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalEnvironmentMapID is hashed by IDENTITY — a change must repopulate the blackboard.";
        }

        // And the teardown case: a scene with NO environment map clears the IDs to 0. That is
        // the transition that leaves the graph holding freed textures if it does not invalidate.
        EXPECT_NE(Access::FingerprintWithIBL({}), baseFp)
            << "Clearing the IBL (scene with no EnvironmentMap) must repopulate the blackboard — "
               "otherwise the graph keeps importing the destroyed scene's textures.";

        // Stability: the same IDs must produce the same fingerprint, or the blackboard would
        // repopulate every frame and the cache would be pointless.
        EXPECT_EQ(Access::FingerprintWithIBL(base), baseFp);
    }

    // A render-stream pass whose Setup() declares nothing when its command
    // bucket is empty (issue #1315).
    //
    // FoliageRenderPass, DecalRenderPass, ForwardOverlayRenderPass and
    // WaterRenderPass all begin `Setup()` with an early return on
    // `GetCommandCount() == 0`. A graph compiled during a frame where one of
    // them had no draws therefore caches a node with no reads and no writes,
    // and the reachability pass culls it. Gaining the first draw moves nothing
    // else in the fingerprint, so BuildFrameGraph keeps serving that cached
    // build, Setup() never runs again, and the pass stays culled while its
    // bucket fills every frame.
    //
    // #1315 is what that costs. A tessellated-terrain evidence test left the
    // graph cached with an empty foliage bucket; the next test in the same
    // process generated 1310 plant instances, culled them and submitted two
    // draws per frame, and drew none of them until an unrelated
    // rendering-path switch happened to move the fingerprint. The frame was
    // otherwise pixel-plausible — the subject was simply absent — so only the
    // #931 content-mask floor caught it at all.
    //
    // Water was hashed, and groom — a seventh pass with the same shape but a
    // request list instead of a bucket — was hashed by #1246. The three in
    // between were not, because each earlier fix saw only its own pass. This
    // test covers all four bucket-gated ones together so the next pass to
    // adopt that Setup shape is caught by accounting rather than by a golden.
    TEST(RenderGraphFingerprint, EveryBucketGatedStreamPassChangesFingerprintOnFirstDraw)
    {
        using Access = RenderPipelineFingerprintAccess;
        struct Case
        {
            const char* Name;
            Access::Stream Stream;
        };
        const std::vector<Case> cases = {
            { "FoliageRenderPass", Access::Stream::Foliage },
            { "DecalRenderPass", Access::Stream::Decal },
            { "ForwardOverlayRenderPass", Access::Stream::ForwardOverlay },
            { "WaterRenderPass", Access::Stream::Water },
        };
        // GroomRenderPass has the same Setup shape and was hashed by #1246,
        // but its gate is a request list owned by Renderer3D's static state
        // rather than a bucket on the pass, so it cannot be driven from a bare
        // Renderer3DData the way these four can. It is covered by its own
        // fingerprint line, not by this loop. The two fluid passes are out for
        // the same reason — their draw list lives on a sibling pass. See
        // docs/agent-rules/render-graph-setup-declaration-gates.md.

        for (const Case& c : cases)
        {
            const auto fp = Access::FingerprintAcrossFirstDraw(c.Stream);
            EXPECT_NE(fp.Empty, fp.WithDraw)
                << c.Name
                << " gates its Setup() declarations on an empty command bucket, but its "
                   "bucket state is not in ComputeBlackboardFingerprint. The graph keeps the "
                   "cached build in which the pass declared nothing, reachability culls it, and "
                   "everything it was asked to draw is silently absent from the frame (#1315).";

            // Stability: the cache exists to be hit. Recomputing with nothing
            // changed must agree, or the graph would rebuild every frame and
            // the bit above would be paying for itself many times over.
            EXPECT_EQ(fp.Restated, fp.WithDraw) << c.Name;
        }
    }
} // namespace OloEngine::Tests
