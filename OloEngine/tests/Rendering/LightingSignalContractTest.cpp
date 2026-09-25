// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/LightingSignalContract.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTTechnique.h"

#include <array>

// =============================================================================
// The lighting-signal ownership ratchet — issue #1336.
//
// ResolveLightingSignalOwnership is what RenderPipeline configures the AO,
// SSGI and hybrid hand-offs from. This file checks it over EVERY combination
// of live techniques on every path, not over a scene someone picked: a term
// with no owner is a dropped term, two owners that are not a declared mixture
// or light partition is a double count, and neither may be reachable from any
// configuration. The GLSL half of the contract — that the composition applies
// each decision — is LightingSignalContractGpuTest; the prose is
// docs/agent-rules/lighting-signal-contract.md.
// =============================================================================

namespace
{
    using namespace OloEngine;

    constexpr std::array<RenderingPath, 3> kPaths = { RenderingPath::Forward, RenderingPath::ForwardPlus,
                                                      RenderingPath::Deferred };
    constexpr u32 kFlagCount = 8;

    [[nodiscard]] LightingFrameConfiguration MakeFrame(RenderingPath path, u32 flags)
    {
        LightingFrameConfiguration frame{};
        frame.Path = path;
        frame.ReSTIRDIActive = (flags & (1u << 0)) != 0u;
        frame.ReSTIRGIActive = (flags & (1u << 1)) != 0u;
        frame.ReSTIRPTActive = (flags & (1u << 2)) != 0u;
        frame.SSGIRequested = (flags & (1u << 3)) != 0u;
        frame.SSRActive = (flags & (1u << 4)) != 0u;
        frame.RayTracedReflectionActive = (flags & (1u << 5)) != 0u;
        frame.ScreenSpaceAOProduced = (flags & (1u << 6)) != 0u;
        frame.ProbeOrIBLSpecularActive = (flags & (1u << 7)) != 0u;
        return frame;
    }

    // Which estimators may EVER answer for a term. An estimator appearing
    // outside its row is the kind of mistake that reads as plausible light —
    // ReSTIR GI's specular lobe added on top of the reflection tiers was one.
    [[nodiscard]] LightingEstimator AllowedOwners(LightingTerm term)
    {
        using enum LightingEstimator;
        switch (term)
        {
            case LightingTerm::DirectDiffuse:
            case LightingTerm::DirectSpecular:
                return RasterLightLoop | ClusteredTiles | ReSTIRDI;
            case LightingTerm::IndirectDiffuse:
                return AmbientLadder | SSGI | ReSTIRGI | ReSTIRPT;
            case LightingTerm::IndirectSpecular:
                return ReflectionProbesIBL | RayTracedReflection | ScreenSpaceReflection | ReSTIRPT;
            case LightingTerm::Emission:
                return SurfaceEmission;
            case LightingTerm::SurfaceTransmission:
                return SurfaceTransmission;
            case LightingTerm::Count:
                break;
        }
        return None;
    }

    template<typename Fn>
    void ForEveryFrame(Fn&& fn)
    {
        for (const RenderingPath path : kPaths)
        {
            for (u32 flags = 0; flags < (1u << kFlagCount); ++flags)
            {
                fn(MakeFrame(path, flags), flags);
            }
        }
    }
} // namespace

TEST(LightingSignalContract, EveryTermHasAnOwnerAndNoUndeclaredSecondOne)
{
    ForEveryFrame(
        [](const LightingFrameConfiguration& frame, u32 flags)
        {
            const LightingSignalOwnership ownership = ResolveLightingSignalOwnership(frame);
            for (sizet t = 0; t < kLightingTermCount; ++t)
            {
                const auto term = static_cast<LightingTerm>(t);
                const LightingTermOwnership owned = ownership.Of(term);
                const u32 owners = CountEstimators(owned.Owners);

                // The one allowed exception to "an owner": no specular ambient
                // at all (IBL off, no tiers) is a scene without that light,
                // not a dropped estimate.
                // (A hybrid tier "active" on a forward path is no source: the
                // resolver corrects that claim, since there is no G-Buffer.)
                const bool deferred = frame.Path == RenderingPath::Deferred;
                const bool noSpecularSourceAtAll =
                    term == LightingTerm::IndirectSpecular && !(deferred && frame.ReSTIRPTActive) &&
                    !frame.ProbeOrIBLSpecularActive &&
                    (!deferred || (!frame.SSRActive && !frame.RayTracedReflectionActive));
                if (!noSpecularSourceAtAll)
                {
                    EXPECT_GE(owners, 1u) << ToString(term) << " has no owner — a dropped term (flags " << flags
                                          << ", path " << static_cast<u32>(frame.Path) << ")";
                }

                switch (owned.Composition)
                {
                    case TermComposition::Exclusive:
                        EXPECT_LE(owners, 1u) << ToString(term) << " is Exclusive with " << owners
                                              << " owners — a double count (flags " << flags << ")";
                        break;
                    case TermComposition::PartitionedByLight:
                        EXPECT_GE(owners, 2u) << ToString(term) << " claims a light partition with one owner";
                        EXPECT_TRUE(term == LightingTerm::DirectDiffuse || term == LightingTerm::DirectSpecular)
                            << ToString(term) << " is partitioned by light, but only direct light has lights";
                        break;
                    case TermComposition::ConfidenceMixture:
                        EXPECT_GE(owners, 2u) << ToString(term) << " claims a mixture with one owner";
                        EXPECT_TRUE(term == LightingTerm::IndirectDiffuse || term == LightingTerm::IndirectSpecular)
                            << ToString(term) << " is a confidence mixture, but only the indirect terms are estimated "
                                                 "by several competing techniques";
                        break;
                }

                const u32 outside = std::to_underlying(owned.Owners) & ~std::to_underlying(AllowedOwners(term));
                EXPECT_EQ(outside, 0u) << ToString(term) << " is owned by an estimator that does not estimate it "
                                       << "(mask " << outside << ", flags " << flags << ")";
            }
        });
}

TEST(LightingSignalContract, HybridTiersOwnNothingWithoutAGBuffer)
{
    ForEveryFrame(
        [](const LightingFrameConfiguration& frame, u32 flags)
        {
            if (frame.Path == RenderingPath::Deferred)
                return;
            const LightingSignalOwnership ownership = ResolveLightingSignalOwnership(frame);
            constexpr auto kDeferredOnly = LightingEstimator::ReSTIRDI | LightingEstimator::ReSTIRGI |
                                           LightingEstimator::ReSTIRPT | LightingEstimator::SSGI |
                                           LightingEstimator::RayTracedReflection |
                                           LightingEstimator::ScreenSpaceReflection;
            for (const LightingTermOwnership& owned : ownership.Terms)
            {
                EXPECT_EQ(std::to_underlying(owned.Owners) & std::to_underlying(kDeferredOnly), 0u)
                    << "a G-Buffer tier owns a term on a forward path (flags " << flags << ")";
            }
            EXPECT_EQ(ownership.SSGI, SSGIComposition::None);
        });
}

TEST(LightingSignalContract, ScreenSpaceAOIsAmbientVisibilityWhereverTheAmbientTermIsSeparable)
{
    ForEveryFrame(
        [](const LightingFrameConfiguration& frame, u32)
        {
            const ScreenSpaceAOApplication application = ResolveLightingSignalOwnership(frame).ScreenSpaceAO;
            if (!frame.ScreenSpaceAOProduced)
            {
                EXPECT_EQ(application, ScreenSpaceAOApplication::None) << "AO applied with no AO buffer produced";
                return;
            }
            if (frame.Path == RenderingPath::Deferred)
            {
                EXPECT_EQ(application, ScreenSpaceAOApplication::AmbientTermInLighting)
                    << "the deferred path composes the ambient term itself, so the AO buffer must multiply that term "
                       "and not the finished frame (direct light, emission and traced indirect included)";
            }
            else
            {
                EXPECT_EQ(application, ScreenSpaceAOApplication::ComposedColorApproximation)
                    << "the forward paths build the AO buffer after their lighting; the composed-colour multiply is "
                       "the declared approximation there";
            }
        });
}

TEST(LightingSignalContract, ContactShadowsAreTheSunsVisibilityAndExistOnlyWithAGBuffer)
{
    for (const RenderingPath path : kPaths)
    {
        for (const bool requested : { false, true })
        {
            LightingFrameConfiguration frame{};
            frame.Path = path;
            frame.ContactShadowsRequested = requested;
            const ContactShadowApplication application = ResolveLightingSignalOwnership(frame).ContactShadow;
            const bool expectApplied = requested && path == RenderingPath::Deferred;
            EXPECT_EQ(application, expectApplied ? ContactShadowApplication::PrimaryDirectionalLightInLighting
                                                 : ContactShadowApplication::None)
                << "path " << static_cast<u32>(path) << ", requested " << requested;
        }
    }
}

// The contract composes the ReSTIR selectors; it must not contradict them.
TEST(LightingSignalContract, AgreesWithTheReSTIROwnershipSelectors)
{
    for (u32 flags = 0; flags < (1u << 4); ++flags)
    {
        const bool di = (flags & 1u) != 0u;
        const bool gi = (flags & 2u) != 0u;
        const bool pt = (flags & 4u) != 0u;
        const bool ssgi = (flags & 8u) != 0u;

        const ReSTIRPTOwnership selected = SelectReSTIRPTOwnership({
            .DIActive = di,
            .GIActive = gi && !pt,
            .PTActive = pt,
            .SSGIActive = ssgi,
        });

        LightingFrameConfiguration frame{};
        frame.Path = RenderingPath::Deferred;
        frame.ReSTIRDIActive = di;
        frame.ReSTIRGIActive = gi;
        frame.ReSTIRPTActive = pt;
        frame.SSGIRequested = ssgi;
        const LightingSignalOwnership ownership = ResolveLightingSignalOwnership(frame);

        EXPECT_EQ(HasEstimator(ownership.Of(LightingTerm::IndirectDiffuse).Owners, LightingEstimator::ReSTIRPT),
                  selected.PTIndirectDiffuse)
            << "flags " << flags;
        EXPECT_EQ(HasEstimator(ownership.Of(LightingTerm::IndirectSpecular).Owners, LightingEstimator::ReSTIRPT),
                  selected.PTIndirectSpecular)
            << "flags " << flags;
        EXPECT_EQ(HasEstimator(ownership.Of(LightingTerm::IndirectDiffuse).Owners, LightingEstimator::ReSTIRGI),
                  selected.DiffuseFallbacks.ReSTIRGIAtPrimary)
            << "flags " << flags;
        EXPECT_EQ(ownership.SSGI != SSGIComposition::None, selected.DiffuseFallbacks.SSGIComposite)
            << "the contract and SelectIndirectDiffuseSources disagree about whether SSGI composites (flags " << flags
            << ")";
        EXPECT_EQ(HasEstimator(ownership.Of(LightingTerm::DirectDiffuse).Owners, LightingEstimator::ReSTIRDI),
                  selected.DIAtPrimary)
            << "flags " << flags;
    }
}

TEST(LightingSignalContract, EmissionAndTransmissionHaveExactlyTheirOwnEstimator)
{
    ForEveryFrame(
        [](const LightingFrameConfiguration& frame, u32)
        {
            const LightingSignalOwnership ownership = ResolveLightingSignalOwnership(frame);
            EXPECT_EQ(ownership.Of(LightingTerm::Emission),
                      (LightingTermOwnership{ LightingEstimator::SurfaceEmission, TermComposition::Exclusive }));
            EXPECT_EQ(ownership.Of(LightingTerm::SurfaceTransmission),
                      (LightingTermOwnership{ LightingEstimator::SurfaceTransmission, TermComposition::Exclusive }));
        });
}

// ReSTIR GI is a DIFFUSE tier. Its specular lobe used to be added on top of the
// reflection tiers that already answer for indirect specular; the resolve now
// evaluates the diffuse lobe alone, and the contract says why.
TEST(LightingSignalContract, ReSTIRGINeverOwnsIndirectSpecular)
{
    ForEveryFrame(
        [](const LightingFrameConfiguration& frame, u32 flags)
        {
            const LightingSignalOwnership ownership = ResolveLightingSignalOwnership(frame);
            EXPECT_FALSE(HasEstimator(ownership.Of(LightingTerm::IndirectSpecular).Owners, LightingEstimator::ReSTIRGI))
                << "flags " << flags;
        });
}
