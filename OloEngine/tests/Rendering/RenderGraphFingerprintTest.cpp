#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Renderer3DInternal.h"
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

        // The raw GL texture IDs the blackboard IMPORTS (as opposed to declares). They are
        // not settings, so they need their own hook.
        struct ImportedIBL
        {
            u32 Irradiance = 0;
            u32 Prefilter = 0;
            u32 BRDFLut = 0;
            u32 Environment = 0;
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
        const Access::ImportedIBL base{ .Irradiance = 26, .Prefilter = 45, .BRDFLut = 46, .Environment = 12 };
        const u64 baseFp = Access::FingerprintWithIBL(base);

        // Each ID must independently invalidate: a scene switch can change any subset.
        {
            Access::ImportedIBL changed = base;
            changed.Irradiance = 99;
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalIrradianceMapID is imported by raw GL ID — a change must repopulate the blackboard.";
        }
        {
            Access::ImportedIBL changed = base;
            changed.Prefilter = 99;
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalPrefilterMapID is imported by raw GL ID — a change must repopulate the blackboard.";
        }
        {
            Access::ImportedIBL changed = base;
            changed.BRDFLut = 99;
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalBRDFLutMapID is imported by raw GL ID — a change must repopulate the blackboard.";
        }
        {
            Access::ImportedIBL changed = base;
            changed.Environment = 99;
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp)
                << "GlobalEnvironmentMapID is imported by raw GL ID — a change must repopulate the blackboard.";
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
} // namespace OloEngine::Tests
