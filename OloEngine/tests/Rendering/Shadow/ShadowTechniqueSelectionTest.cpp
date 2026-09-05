// OLO_TEST_LAYER: plumbing
//
// #1056 — the shadow-technique seam, and specifically its FALLBACK arm.
//
// Every assertion here runs on a machine with no GPU, which is deliberate:
// the fallback is the path every CI runner takes, and a fallback nobody tests
// is the exact shape of a light that goes silently unshadowed. The house rule
// is that a path which cannot do its job says so loudly and countably; these
// tests are what "countably" means.
//
// WHAT THIS CANNOT SEE, stated because a substitution is a decision about what
// you stop testing (substituted-seams-compound.md): it cannot see a wrong ray
// offset, a stale acceleration structure, a mask sampled at the wrong channel,
// or a routing lane that reached the GPU with the wrong value. Those are
// device facts, pinned by the visual-evidence captures and the live editor
// run — never here.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Shadow/ShadowTechnique.h"

#include <array>
#include <cstddef>

namespace OloEngine::Tests
{
    namespace
    {
        // Everything ray tracing needs, all present. Each test then knocks out
        // exactly ONE input, so the reason it asserts on is the reason that
        // input is responsible for and not a second failure riding along.
        constexpr ShadowTechniqueInputs MakeReadyInputs()
        {
            return ShadowTechniqueInputs{
                .Requested = ShadowTechnique::RayTraced,
                .LightCastsShadows = true,
                .DeferredPathActive = true,
                .RayTracingAvailable = true,
                .TlasReady = true,
                .MaskAvailable = true,
            };
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The happy path
    // -------------------------------------------------------------------------

    TEST(ShadowTechniqueSelection, AReadyLightGetsRayTracingAndTheFirstFreeChannel)
    {
        const auto decision = SelectShadowTechnique(MakeReadyInputs(), 0);

        EXPECT_TRUE(decision.IsRayTraced());
        EXPECT_EQ(decision.Effective, ShadowTechnique::RayTraced);
        EXPECT_EQ(decision.Reason, ShadowTechniqueFallbackReason::None);
        EXPECT_EQ(decision.MaskChannel, 0);
    }

    TEST(ShadowTechniqueSelection, ChannelsAreHandedOutInOrder)
    {
        for (u32 taken = 0; taken < kRayTracedShadowMaskChannels; ++taken)
        {
            const auto decision = SelectShadowTechnique(MakeReadyInputs(), taken);
            EXPECT_TRUE(decision.IsRayTraced()) << "channel " << taken;
            EXPECT_EQ(decision.MaskChannel, static_cast<i32>(taken));
        }
    }

    // The decision must be constexpr-evaluable — not for speed, but because a
    // pure function is what makes it testable at all, and a static_assert is
    // the cheapest guard against someone reaching for renderer state inside it.
    TEST(ShadowTechniqueSelection, TheDecisionIsAPureCompileTimeFunction)
    {
        static constexpr auto decision = SelectShadowTechnique(MakeReadyInputs(), 0);
        static_assert(decision.Effective == ShadowTechnique::RayTraced);
        static_assert(decision.MaskChannel == 0);
        SUCCEED();
    }

    // -------------------------------------------------------------------------
    // Every fallback reason, one input at a time
    // -------------------------------------------------------------------------

    TEST(ShadowTechniqueSelection, NoDeviceFallsBackWithTheDeviceReason)
    {
        auto inputs = MakeReadyInputs();
        inputs.RayTracingAvailable = false;

        const auto decision = SelectShadowTechnique(inputs, 0);

        EXPECT_FALSE(decision.IsRayTraced());
        EXPECT_EQ(decision.Effective, ShadowTechnique::ShadowMap);
        EXPECT_EQ(decision.Reason, ShadowTechniqueFallbackReason::RayTracingUnavailable);
        EXPECT_EQ(decision.MaskChannel, kNoRayTracedShadowChannel);
    }

    // The OLO_VULKAN_NO_RAY_TRACING=1 lever and a GPU without the extensions
    // arrive at this function as the same input — RayTracingScene::IsAvailable()
    // is false either way — so the lever needs no separate arm here. What the
    // lever changes is WHICH RayTracing::UnsupportedReason the capability
    // carries, and that is pinned by #978's own tests.
    TEST(ShadowTechniqueSelection, AnEmptyAccelerationStructureIsDistinctFromNoDevice)
    {
        auto inputs = MakeReadyInputs();
        inputs.TlasReady = false;

        const auto decision = SelectShadowTechnique(inputs, 0);

        EXPECT_EQ(decision.Reason, ShadowTechniqueFallbackReason::AccelerationStructureEmpty);
        // The distinction is the whole point: "the first frame has no TLAS yet"
        // and "this GPU cannot ray trace" would otherwise be one indistinct
        // zero, and a user would read the former as the latter.
        EXPECT_NE(decision.Reason, ShadowTechniqueFallbackReason::RayTracingUnavailable);
    }

    TEST(ShadowTechniqueSelection, ForwardPathsFallBackBecauseThereIsNoGBuffer)
    {
        auto inputs = MakeReadyInputs();
        inputs.DeferredPathActive = false;

        EXPECT_EQ(SelectShadowTechnique(inputs, 0).Reason,
                  ShadowTechniqueFallbackReason::RenderingPathUnsupported);
    }

    TEST(ShadowTechniqueSelection, AMissingMaskFallsBackRatherThanSamplingNothing)
    {
        auto inputs = MakeReadyInputs();
        inputs.MaskAvailable = false;

        EXPECT_EQ(SelectShadowTechnique(inputs, 0).Reason, ShadowTechniqueFallbackReason::MaskUnavailable);
    }

    TEST(ShadowTechniqueSelection, TheFifthLightExhaustsTheChannelBudget)
    {
        const auto decision = SelectShadowTechnique(MakeReadyInputs(), kRayTracedShadowMaskChannels);

        EXPECT_FALSE(decision.IsRayTraced());
        EXPECT_EQ(decision.Reason, ShadowTechniqueFallbackReason::MaskChannelBudgetExhausted);
        // Never a reused channel: a fifth light silently sharing light 0's
        // channel would shadow it with the WRONG light's visibility, which
        // looks like a plausible image and is wrong.
        EXPECT_EQ(decision.MaskChannel, kNoRayTracedShadowChannel);
    }

    TEST(ShadowTechniqueSelection, ANonCastingLightIsReportedSeparatelyFromAFallback)
    {
        auto inputs = MakeReadyInputs();
        inputs.LightCastsShadows = false;

        EXPECT_EQ(SelectShadowTechnique(inputs, 0).Reason, ShadowTechniqueFallbackReason::LightNotShadowCasting);
    }

    TEST(ShadowTechniqueSelection, NotAskingForRayTracingIsNotAFallback)
    {
        auto inputs = MakeReadyInputs();
        inputs.Requested = ShadowTechnique::ShadowMap;

        EXPECT_EQ(SelectShadowTechnique(inputs, 0).Reason, ShadowTechniqueFallbackReason::NotRequested);
    }

    // The ORDER of the guards is a contract, not an implementation detail: a
    // light that trips several is reported by the most fundamental one, so the
    // dominant reason a user is shown names the thing they can actually fix.
    TEST(ShadowTechniqueSelection, TheMostFundamentalReasonWinsWhenSeveralApply)
    {
        ShadowTechniqueInputs inputs{
            .Requested = ShadowTechnique::RayTraced,
            .LightCastsShadows = true,
            .DeferredPathActive = false,
            .RayTracingAvailable = false,
            .TlasReady = false,
            .MaskAvailable = false,
        };

        EXPECT_EQ(SelectShadowTechnique(inputs, 0).Reason,
                  ShadowTechniqueFallbackReason::RenderingPathUnsupported);

        // A light that casts nothing outranks even that: neither technique runs
        // for it, so counting it as a ray-tracing fallback would inflate the
        // fallback total with lights that were never going to trace a ray.
        inputs.LightCastsShadows = false;
        EXPECT_EQ(SelectShadowTechnique(inputs, 0).Reason,
                  ShadowTechniqueFallbackReason::LightNotShadowCasting);
    }

    // Every reason must produce a distinct, non-empty sentence — a counter whose
    // reason string is "unknown" is a counter nobody can act on, which is the
    // failure this whole enum exists to prevent.
    TEST(ShadowTechniqueSelection, EveryReasonHasItsOwnSentence)
    {
        std::array<std::string_view, static_cast<sizet>(ShadowTechniqueFallbackReason::Count)> seen{};
        for (sizet i = 0; i < seen.size(); ++i)
        {
            const auto reason = static_cast<ShadowTechniqueFallbackReason>(i);
            seen[i] = ToString(reason);
            EXPECT_FALSE(seen[i].empty());
            EXPECT_NE(seen[i], "unknown") << "reason " << i << " has no sentence";
        }
        for (sizet i = 0; i < seen.size(); ++i)
        {
            for (sizet j = i + 1; j < seen.size(); ++j)
                EXPECT_NE(seen[i], seen[j]) << "reasons " << i << " and " << j << " share a sentence";
        }
    }

    // -------------------------------------------------------------------------
    // The counters
    // -------------------------------------------------------------------------

    TEST(ShadowTechniqueStatsTest, FallbackLightsCountsOnlyGenuineFailures)
    {
        ShadowTechniqueStats stats;

        stats.Record(SelectShadowTechnique(MakeReadyInputs(), 0));

        auto noDevice = MakeReadyInputs();
        noDevice.RayTracingAvailable = false;
        stats.Record(SelectShadowTechnique(noDevice, 1));

        auto notRequested = MakeReadyInputs();
        notRequested.Requested = ShadowTechnique::ShadowMap;
        stats.Record(SelectShadowTechnique(notRequested, 1));

        auto notCasting = MakeReadyInputs();
        notCasting.LightCastsShadows = false;
        stats.Record(SelectShadowTechnique(notCasting, 1));

        EXPECT_EQ(stats.RayTracedLights, 1u);
        // Only the no-device light. A light that never asked, and a light that
        // casts no shadow at all, are not failures to deliver something — and a
        // FallbackLights that counted them would read as broken on every scene
        // that simply does not use the feature.
        EXPECT_EQ(stats.FallbackLights, 1u);
        EXPECT_EQ(stats.ByReason[static_cast<sizet>(ShadowTechniqueFallbackReason::None)], 1u);
        EXPECT_EQ(stats.ByReason[static_cast<sizet>(ShadowTechniqueFallbackReason::NotRequested)], 1u);
        EXPECT_EQ(stats.ByReason[static_cast<sizet>(ShadowTechniqueFallbackReason::LightNotShadowCasting)], 1u);
        EXPECT_EQ(stats.ByReason[static_cast<sizet>(ShadowTechniqueFallbackReason::RayTracingUnavailable)], 1u);
    }

    TEST(ShadowTechniqueStatsTest, TheDominantReasonIgnoresTheNonFailures)
    {
        ShadowTechniqueStats stats;

        // Ten lights that never asked, and two that asked and could not be
        // served. The reported reason must be the one a user can act on.
        auto notRequested = MakeReadyInputs();
        notRequested.Requested = ShadowTechnique::ShadowMap;
        for (int i = 0; i < 10; ++i)
            stats.Record(SelectShadowTechnique(notRequested, 0));

        auto noTlas = MakeReadyInputs();
        noTlas.TlasReady = false;
        stats.Record(SelectShadowTechnique(noTlas, 0));
        stats.Record(SelectShadowTechnique(noTlas, 0));

        EXPECT_EQ(stats.DominantFallbackReason(), ShadowTechniqueFallbackReason::AccelerationStructureEmpty);
    }

    TEST(ShadowTechniqueStatsTest, NoFallbacksReportsNoReason)
    {
        ShadowTechniqueStats stats;
        stats.Record(SelectShadowTechnique(MakeReadyInputs(), 0));

        EXPECT_EQ(stats.DominantFallbackReason(), ShadowTechniqueFallbackReason::None);
        EXPECT_EQ(stats.FallbackLights, 0u);
    }

    TEST(ShadowTechniqueStatsTest, ResetClearsEveryLane)
    {
        ShadowTechniqueStats stats;
        stats.Record(SelectShadowTechnique(MakeReadyInputs(), 0));
        stats.MaskedOccludersShadowedAsSolid = 7;

        stats.Reset();

        EXPECT_EQ(stats, ShadowTechniqueStats{});
    }

    // -------------------------------------------------------------------------
    // The settings and the GPU mirror
    // -------------------------------------------------------------------------

    TEST(ShadowTechniqueSettings, RasterIsTheDefaultTechnique)
    {
        // #979 keeps shadow maps as a permanent tier and the fallback. A default
        // that silently switched every scene to ray tracing would also silently
        // change what a no-RT machine renders, which is the opposite of the
        // guarantee.
        const ShadowSettings settings{};
        EXPECT_EQ(settings.Technique, ShadowTechnique::ShadowMap);
        EXPECT_TRUE(settings.RayTraced.TemporalAccumulation);
        EXPECT_TRUE(settings.RayTraced.SpatialFilter);
        EXPECT_EQ(settings.RayTraced.RaysPerPixel, 1u);
    }

    TEST(ShadowTechniqueSettings, TheMaskChannelBudgetMatchesTheTargetItPacksInto)
    {
        // Four is the channel count of one RGBA target, not a tuning knob.
        // Raising it without adding a target would make lights 5.. read past
        // the vec4 the shader indexes, which GLSL clamps rather than errors on —
        // a wrong shadow, not a crash.
        EXPECT_EQ(kRayTracedShadowMaskChannels, 4u);
        // The routing lane the shader indexes is one ivec4, so it can address
        // exactly this many channels and no more.
        const UBOStructures::ShadowUBO ubo{};
        EXPECT_EQ(static_cast<u32>(ubo.RayTracedShadowLightIndices.length()), kRayTracedShadowMaskChannels);
    }

    TEST(ShadowTechniqueSettings, TheShadowUBOShipsTheRoutingDisabledAndUnassigned)
    {
        // The default IS the fallback. RayTracedShadowPass turns the branch on
        // after its draws, so a frame where it never ran leaves the lighting
        // shader on the raster path by construction rather than by remembering
        // to reset a flag.
        const UBOStructures::ShadowUBO ubo{};
        EXPECT_FLOAT_EQ(ubo.RayTracedShadowParams.x, 0.0f);
        EXPECT_EQ(ubo.RayTracedShadowLightIndices.x, kNoRayTracedShadowChannel);
        EXPECT_EQ(ubo.RayTracedShadowLightIndices.y, kNoRayTracedShadowChannel);
        EXPECT_EQ(ubo.RayTracedShadowLightIndices.z, kNoRayTracedShadowChannel);
        EXPECT_EQ(ubo.RayTracedShadowLightIndices.w, kNoRayTracedShadowChannel);
    }

    // The GLSL reference block and the C++ struct must agree, or every field
    // after the drift reads garbage — and a garbage shadow routing is a
    // plausible-looking image, not an error.
    TEST(ShadowTechniqueSettings, TheShadowUBOGlslReferenceCarriesTheRoutingLanes)
    {
        const std::string layout = ShaderBindingLayout::GetShadowUBOLayout();
        EXPECT_NE(layout.find("u_RayTracedShadowLightIndices"), std::string::npos);
        EXPECT_NE(layout.find("u_RayTracedShadowParams"), std::string::npos);
    }

    TEST(ShadowTechniqueSettings, TheRayTracingShadowUBOMatchesItsGlslBlock)
    {
        // 400 B: 3 mat4 + 2 vec4[4] + uvec4 + 4 vec4. The static_assert in
        // ShaderBindingLayout.h is the real guard; this repeats the number
        // where a reader of the test suite can see it, and fails loudly if
        // someone "fixes" the assert instead of the layout.
        EXPECT_EQ(UBOStructures::RayTracingShadowUBO::GetSize(), 400u);
        EXPECT_EQ(UBOStructures::RayTracingShadowUBO::GetSize() % 16u, 0u);
    }

    TEST(ShadowTechniqueSettings, TheMaskTextureSlotIsBelowTheGLCombinedUnitFloor)
    {
        // The mask claimed a new engine texture slot and pushed the shader-graph
        // user base up by one. GL 4.6 only guarantees 80 combined units, and
        // going over is a silent unbound sampler on the OpenGL backend.
        EXPECT_LT(ShaderBindingLayout::TEX_RAY_TRACED_SHADOW, ShaderBindingLayout::TEX_SHADER_GRAPH_0);
        EXPECT_LT(ShaderBindingLayout::TEX_SHADER_GRAPH_0, 80u);
        EXPECT_TRUE(ShaderBindingLayout::IsKnownTextureBinding(ShaderBindingLayout::TEX_RAY_TRACED_SHADOW,
                                                               "u_RayTracedShadowMask"));
        // The pass's params block rides UBO_RAY_TRACING (65) alongside the #978
        // probe's own block; the validator has to accept both names.
        EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_RAY_TRACING,
                                                           "RayTracingShadowParams"));
    }

    // -------------------------------------------------------------------------
    // The per-light request
    // -------------------------------------------------------------------------

    TEST(RayTracedShadowLightRequestTest, AnUnsetRequestNamesNoLight)
    {
        const RayTracedShadowLightRequest request{};
        // -1 rather than 0: light 0 is the primary directional light, so a
        // default-constructed request that named light 0 would silently route
        // the sun's channel to a light nobody asked about.
        EXPECT_EQ(request.UboLightIndex, -1);
        EXPECT_FALSE(request.Directional);
        EXPECT_FLOAT_EQ(request.Shape, 0.0f);
        EXPECT_FLOAT_EQ(request.Range, 0.0f);
    }
} // namespace OloEngine::Tests
