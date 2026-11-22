// OLO_TEST_LAYER: plumbing
//
// Whole-renderer memory admission (issue #1343), the policy half.
//
// RendererMemoryBudget.h is plain data and pure functions, so everything here runs with no
// device, on every CI runner. The governor is driven by a small simulated renderer whose
// resident bytes follow the configuration it was told to realize, with the two-frame
// retirement delay both backends have: an old resource stays resident after its
// replacement was allocated.
//
// What it pins:
//   * the ladder: its order, that every rung is a no-op where it has nothing to remove,
//     that a deeper level never holds more than a shallower one;
//   * the footprint model: the G-Buffer to the byte, history layouts, a resize rescaling
//     measured owners, "not measured yet" reported as incomplete rather than as 0;
//   * admission: unknown budget, ample budget, pressure -> the first level that fits,
//     with a stable reason and rung list, identical across two replays;
//   * the transition: a descent that allocates is staged when its overlap does not fit,
//     a resize is admitted before its targets exist, peak resident stays inside the
//     prediction;
//   * recovery: only after a stable run, never on a pulse shorter than it, and a climb
//     that had to be undone is learned from instead of repeated;
//   * the floor: the minimum complete image is kept and reported as over budget;
//   * two views share one budget.
//
// The GPU half -- real frames under an injected budget on OpenGL and Vulkan -- is
// RendererMemoryAdmissionEvidenceTest.cpp.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RendererMemoryBudget.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u64 kMiB = 1024ull * 1024ull;

        // G-Buffer bytes per texel at one sample: the attachment table plus depth.
        [[nodiscard]] u64 GBufferTexelBytes()
        {
            return GBuffer::EstimateBytes(1u, 1u, 1u).value_or(0u);
        }

        [[nodiscard]] MemoryQualityConfig MakeConfig(const RenderingPath path, const u32 width, const u32 height,
                                                     const u32 msaa = 1u, const u32 techniques = 0u)
        {
            MemoryQualityConfig config;
            config.Path = path;
            config.DisplayWidth = width;
            config.DisplayHeight = height;
            config.SceneWidth = width;
            config.SceneHeight = height;
            config.MsaaSamples = msaa;
            config.Techniques = techniques;
            // A scene without representation streaming has no optional detail to give back.
            config.OptionalDetail = false;
            return config;
        }

        // A test policy with short clocks; the defaults are tuned for a real frame rate.
        [[nodiscard]] MemoryAdmissionPolicy TestPolicy()
        {
            MemoryAdmissionPolicy policy;
            policy.ReserveBytes = 16ull * kMiB;
            policy.ReserveFraction = 0.0f;
            policy.RecoveryBandFraction = 0.05f;
            policy.RecoveryFrames = 8;
            policy.SettleFrames = 3;
            policy.MaxRecoveryBackoffShift = 3;
            return policy;
        }

        // Resident bytes that follow what the governor decided, the way a frame would:
        // the new configuration's storage is allocated at once, the old one's is released
        // into a queue that frees it two frames later.
        class SimulatedRenderer
        {
          public:
            // Everything the governor does not control: assets, shadow maps, IBL.
            u64 BaseBytes = 512ull * kMiB;
            // Pass targets and transients per display pixel (measured owners).
            u64 NodeBytesPerPixel = 24;
            u64 TransientBytesPerPixel = 68;
            // Bytes each ON technique holds that the model does not know of.
            u64 HiddenBytesPerTechnique = 0;
            // Optional groom / vegetation detail: resident while any view keeps it.
            u64 OptionalDetailBytes = 0;

            void AddView(const u64 id, const MemoryQualityConfig& requested)
            {
                MemoryAdmissionViewInput view;
                view.View = id;
                view.Requested = requested;
                m_Views.Add(view);
            }

            void RemoveView(const u64 id)
            {
                for (i32 index = 0; index < m_Views.Num(); ++index)
                {
                    if (m_Views[index].View != id)
                        continue;
                    if (m_Views[index].Allocated)
                        Retire(Footprint(*m_Views[index].Allocated));
                    m_Views.RemoveAt(index);
                    return;
                }
            }

            [[nodiscard]] MemoryQualityConfig& Requested(const u64 id)
            {
                for (auto& view : m_Views)
                {
                    if (view.View == id)
                        return view.Requested;
                }
                ADD_FAILURE() << "no such view";
                return m_Views[0].Requested;
            }

            [[nodiscard]] u64 LiveBytes() const
            {
                u64 live = BaseBytes + (m_OptionalResident ? OptionalDetailBytes : 0u);
                for (const auto& view : m_Views)
                {
                    if (view.Allocated)
                        live += Footprint(*view.Allocated);
                }
                return live;
            }

            [[nodiscard]] u64 RetiringBytes() const
            {
                return m_Retiring[0] + m_Retiring[1];
            }

            [[nodiscard]] u64 ResidentBytes() const
            {
                return LiveBytes() + RetiringBytes();
            }

            [[nodiscard]] MemoryAdmissionInput Input(const std::optional<u64> budget) const
            {
                MemoryAdmissionInput input;
                input.Budget.BudgetBytes = budget;
                input.Budget.Source = budget ? MemoryBudgetSource::Injected : MemoryBudgetSource::None;
                input.Budget.ResidentBytes = ResidentBytes();
                input.Budget.RetiringBytes = RetiringBytes();
                input.Views = m_Views;
                input.OptionalDetailBytes = m_OptionalResident ? OptionalDetailBytes : 0u;
                input.KnownOptionalDetailBytes = OptionalDetailBytes;
                return input;
            }

            // One frame: the oldest retired storage is freed, then every view is rebuilt
            // for what the governor realized.
            void Render(const MemoryAdmissionSnapshot& decision)
            {
                m_Retiring[0] = m_Retiring[1];
                m_Retiring[1] = 0;
                for (auto& view : m_Views)
                {
                    const MemoryQualityConfig* realized = nullptr;
                    for (const auto& candidate : decision.Views)
                    {
                        if (candidate.View == view.View)
                            realized = &candidate.Realized;
                    }
                    ASSERT_NE(realized, nullptr) << "the decision does not cover view " << view.View;
                    if (view.Allocated && *view.Allocated == *realized)
                        continue;
                    if (view.Allocated)
                    {
                        // Only what the transition replaces retires; the rest is reused.
                        const u64 before = Footprint(*view.Allocated);
                        const u64 kept = KeptBytes(*view.Allocated, *realized);
                        Retire(before - kept);
                    }
                    view.Allocated = *realized;
                    view.Inventory = Measure(*realized);
                }
                bool wanted = false;
                for (const auto& view : m_Views)
                    wanted = wanted || (view.Allocated && view.Allocated->OptionalDetail);
                if (m_OptionalResident && !wanted)
                    Retire(OptionalDetailBytes);
                m_OptionalResident = wanted;
                m_PeakResident = std::max(m_PeakResident, ResidentBytes());
            }

            void ResetPeak()
            {
                m_PeakResident = ResidentBytes();
            }
            [[nodiscard]] u64 PeakResident() const
            {
                return m_PeakResident;
            }

            [[nodiscard]] u64 Footprint(const MemoryQualityConfig& config) const
            {
                const u64 pixels = static_cast<u64>(config.DisplayWidth) * config.DisplayHeight;
                u64 bytes = EstimateGBufferBytes(config) + pixels * (NodeBytesPerPixel + TransientBytesPerPixel);
                for (u32 index = 0; index < kMemoryTechniqueCount; ++index)
                {
                    const auto technique = static_cast<MemoryTechnique>(index);
                    if (config.Has(technique))
                        bytes += EstimateTechniqueHistoryBytes(technique, config) + HiddenBytesPerTechnique;
                }
                return bytes;
            }

          private:
            // Storage a transition from `from` to `to` leaves in place.
            [[nodiscard]] u64 KeptBytes(const MemoryQualityConfig& from, const MemoryQualityConfig& to) const
            {
                const bool sameExtents = from.DisplayWidth == to.DisplayWidth && from.DisplayHeight == to.DisplayHeight &&
                                         from.SceneWidth == to.SceneWidth && from.SceneHeight == to.SceneHeight;
                if (!sameExtents)
                    return 0;
                const u64 pixels = static_cast<u64>(from.DisplayWidth) * from.DisplayHeight;
                u64 kept = pixels * (NodeBytesPerPixel + TransientBytesPerPixel);
                if (from.Path == to.Path && from.MsaaSamples == to.MsaaSamples)
                    kept += EstimateGBufferBytes(from);
                for (u32 index = 0; index < kMemoryTechniqueCount; ++index)
                {
                    const auto technique = static_cast<MemoryTechnique>(index);
                    if (from.Has(technique) && to.Has(technique) &&
                        EstimateTechniqueHistoryBytes(technique, from) == EstimateTechniqueHistoryBytes(technique, to))
                        kept += EstimateTechniqueHistoryBytes(technique, from) + HiddenBytesPerTechnique;
                }
                return kept;
            }

            [[nodiscard]] ViewMemoryInventory Measure(const MemoryQualityConfig& config) const
            {
                const u64 pixels = static_cast<u64>(config.DisplayWidth) * config.DisplayHeight;
                ViewMemoryInventory inventory;
                inventory.DisplayWidth = config.DisplayWidth;
                inventory.DisplayHeight = config.DisplayHeight;
                inventory.NodeTargetBytes = pixels * NodeBytesPerPixel;
                inventory.TransientDemandBytes = pixels * TransientBytesPerPixel;
                for (u32 index = 0; index < kMemoryTechniqueCount; ++index)
                {
                    const auto technique = static_cast<MemoryTechnique>(index);
                    if (config.Has(technique))
                        inventory.TechniqueHistoryBytes[index] = EstimateTechniqueHistoryBytes(technique, config);
                }
                return inventory;
            }

            void Retire(const u64 bytes)
            {
                m_Retiring[1] += bytes;
            }

            TArray<MemoryAdmissionViewInput> m_Views;
            // [0] is freed by the next frame, [1] by the one after.
            std::array<u64, 2> m_Retiring{};
            u64 m_PeakResident = 0;
            bool m_OptionalResident = false;
        };

        struct Step
        {
            u32 Level = 0;
            MemoryAdmissionReason Reason = MemoryAdmissionReason::Admitted;
            u32 Rungs = 0;

            auto operator==(const Step&) const -> bool = default;
        };

        // Ticks the governor and renders its decision, `frames` times.
        std::vector<Step> Drive(RendererMemoryGovernor& governor, SimulatedRenderer& renderer, const std::optional<u64> budget,
                                const u32 frames)
        {
            std::vector<Step> steps;
            for (u32 frame = 0; frame < frames; ++frame)
            {
                const MemoryAdmissionSnapshot& decision = governor.Tick(renderer.Input(budget));
                steps.push_back({ decision.Level, decision.Reason, decision.ActiveRungMask });
                renderer.Render(decision);
            }
            return steps;
        }

        [[nodiscard]] u32 RungBit(const std::string_view name)
        {
            for (u32 rung = 0; rung < kMemoryLadderRungCount; ++rung)
            {
                if (name == kMemoryLadder[rung].Name)
                    return 1u << rung;
            }
            ADD_FAILURE() << "no rung named " << name;
            return 0;
        }

        constexpr u32 kTechSSGI = MemoryTechniqueBit(MemoryTechnique::SSGI);
        constexpr u32 kTechSSR = MemoryTechniqueBit(MemoryTechnique::SSR);
        constexpr u32 kTechBloom = MemoryTechniqueBit(MemoryTechnique::Bloom);
        constexpr u32 kTechReSTIRGI = MemoryTechniqueBit(MemoryTechnique::ReSTIRGI);
    } // namespace

    // ---------------------------------------------------------------------------------
    // The ladder
    // ---------------------------------------------------------------------------------

    TEST(RendererMemoryLadder, EveryRungHasAStableUniqueName)
    {
        std::set<std::string> names;
        for (const auto& rung : kMemoryLadder)
        {
            ASSERT_NE(rung.Name, nullptr);
            EXPECT_FALSE(std::string_view(rung.Name).empty());
            EXPECT_TRUE(names.insert(rung.Name).second) << rung.Name << " names two rungs";
        }
        // Every optional technique but the path tracer is a rung, each exactly once.
        std::set<u8> techniques;
        for (const auto& rung : kMemoryLadder)
        {
            if (rung.Kind == MemoryReductionKind::TechniqueFallback)
                EXPECT_TRUE(techniques.insert(rung.Argument).second);
        }
        EXPECT_EQ(techniques.size(), kMemoryTechniqueCount - 1u);
        EXPECT_FALSE(techniques.contains(static_cast<u8>(MemoryTechnique::PathTracer)))
            << "the path tracer replaces the lit image; it is counted, never removed";
    }

    TEST(RendererMemoryLadder, ARungWithNothingToRemoveChangesNothing)
    {
        // A forward view with no optional technique: only the pool slack and the optional
        // detail are reducible.
        MemoryQualityConfig forward = MakeConfig(RenderingPath::Forward, 1280u, 720u, 8u);
        forward.OptionalDetail = true;
        u32 mask = 0;
        const MemoryQualityConfig floor = ReduceMemoryQuality(forward, kMemoryLadderRungCount, &mask);
        EXPECT_EQ(mask, RungBit("transientPoolSlack") | RungBit("optionalDetailPools"));
        EXPECT_EQ(floor.MsaaSamples, 8u) << "MSAA is the Deferred G-Buffer's; a forward path has no samples to remove";
        EXPECT_EQ(floor.Path, forward.Path);
        EXPECT_EQ(floor.DisplayWidth, forward.DisplayWidth);
        EXPECT_EQ(floor.SceneWidth, forward.SceneWidth);
    }

    TEST(RendererMemoryLadder, RungsApplyInTheDocumentedOrder)
    {
        MemoryQualityConfig requested = MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 8u, kTechSSGI | kTechSSR | kTechBloom);
        requested.SSGIHalfResolution = false;
        requested.OptionalDetail = true;

        EXPECT_EQ(ReduceMemoryQuality(requested, 0u), requested);

        const MemoryQualityConfig slack = ReduceMemoryQuality(requested, 1u);
        EXPECT_EQ(slack.TransientPoolSpare, 0u);
        EXPECT_EQ(slack.MsaaSamples, 8u);

        const MemoryQualityConfig halfHistory = ReduceMemoryQuality(requested, 2u);
        EXPECT_TRUE(halfHistory.SSGIHalfResolution);
        EXPECT_EQ(halfHistory.MsaaSamples, 8u);

        EXPECT_EQ(ReduceMemoryQuality(requested, 3u).MsaaSamples, 4u);
        EXPECT_EQ(ReduceMemoryQuality(requested, 4u).MsaaSamples, 2u);
        const MemoryQualityConfig noMsaa = ReduceMemoryQuality(requested, 5u);
        EXPECT_EQ(noMsaa.MsaaSamples, 1u);
        EXPECT_TRUE(noMsaa.OptionalDetail);
        EXPECT_EQ(noMsaa.Techniques, requested.Techniques) << "samples go before any technique does";

        u32 mask = 0;
        const MemoryQualityConfig floor = ReduceMemoryQuality(requested, kMemoryLadderRungCount, &mask);
        EXPECT_EQ(floor.Techniques, 0u);
        EXPECT_FALSE(floor.OptionalDetail);
        // The minimum complete image keeps the path and both resolutions.
        EXPECT_EQ(floor.Path, RenderingPath::Deferred);
        EXPECT_EQ(floor.DisplayWidth, 1920u);
        EXPECT_EQ(floor.SceneHeight, 1080u);
        EXPECT_EQ(mask & RungBit("ssgiFallback"), RungBit("ssgiFallback"));
        EXPECT_EQ(mask & RungBit("restirGIFallback"), 0u) << "a technique that was never on is not reported as removed";
    }

    TEST(RendererMemoryLadder, ADeeperLevelNeverHoldsMoreThanAShallowerOne)
    {
        MemoryQualityConfig requested = MakeConfig(RenderingPath::Deferred, 2560u, 1440u, 8u,
                                                   kTechSSGI | kTechSSR | kTechReSTIRGI | kTechBloom);
        requested.SSGIHalfResolution = false;
        SimulatedRenderer renderer;
        u64 previous = renderer.Footprint(requested);
        for (u32 level = 1; level <= kMemoryLadderRungCount; ++level)
        {
            const u64 bytes = renderer.Footprint(ReduceMemoryQuality(requested, level));
            EXPECT_LE(bytes, previous) << "level " << level << " (" << kMemoryLadder[level - 1u].Name << ") grew the footprint";
            previous = bytes;
        }
    }

    // ---------------------------------------------------------------------------------
    // The footprint model
    // ---------------------------------------------------------------------------------

    TEST(RendererMemoryFootprint, TheGBufferIsPricedFromItsOwnAttachmentTable)
    {
        const u64 texel = GBufferTexelBytes();
        ASSERT_GT(texel, 0u);
        // The table: five colour lanes and depth. A change to it moves this on purpose.
        u64 expected = RendererMemoryFormat::BytesPerTexel(FramebufferTextureFormat::Depth).value_or(0u);
        for (const auto format : GBuffer::s_ColorAttachmentFormats)
            expected += RendererMemoryFormat::BytesPerTexel(format).value_or(0u);
        EXPECT_EQ(texel, expected);

        const u64 pixels = 1920ull * 1080ull;
        EXPECT_EQ(EstimateGBufferBytes(MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 1u)), pixels * texel);
        // MSAA: every sample, plus the single-sample resolve target beside it.
        EXPECT_EQ(EstimateGBufferBytes(MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 4u)), pixels * texel * 5u);
        EXPECT_EQ(EstimateGBufferBytes(MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 8u)), pixels * texel * 9u);
        // No G-Buffer off the Deferred path, whatever the sample setting says.
        EXPECT_EQ(EstimateGBufferBytes(MakeConfig(RenderingPath::Forward, 1920u, 1080u, 8u)), 0u);
        EXPECT_EQ(EstimateGBufferBytes(MakeConfig(RenderingPath::ForwardPlus, 1920u, 1080u, 8u)), 0u);
    }

    TEST(RendererMemoryFootprint, HistoryLayoutsFollowTheirBand)
    {
        MemoryQualityConfig config = MakeConfig(RenderingPath::Deferred, 1920u, 1080u);
        const u64 pixels = 1920ull * 1080ull;
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::SSR, config), pixels * 8u);
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::RayTracedShadows, config), pixels * 8u * 3u);
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::ReSTIRDI, config), pixels * 16u * 5u);
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::ReSTIRGI, config), pixels * 16u * 5u);
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::PathTracer, config), pixels * 16u * 4u);
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::Bloom, config), 0u) << "bloom holds transients only";
        // ReSTIR PT's four path-record pools, 592 bytes a pixel each: the largest owner.
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::ReSTIRPT, config), pixels * 592u * 4u);

        // SSGI runs at the trace band: half each way by default, the scene band when not.
        config.SSGIHalfResolution = true;
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::SSGI, config), 960ull * 540ull * 8u * 4u);
        config.SSGIHalfResolution = false;
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::SSGI, config), pixels * 8u * 4u);

        // An upscale preset shrinks the scene band the histories are sized by.
        config.SceneWidth = 960u;
        config.SceneHeight = 540u;
        EXPECT_EQ(EstimateTechniqueHistoryBytes(MemoryTechnique::SSR, config), 960ull * 540ull * 8u);
    }

    TEST(RendererMemoryFootprint, ATransitionChargesTheNewStorageAndReleasesTheOld)
    {
        const u64 texel = GBufferTexelBytes();
        const u64 pixels = 1280ull * 720ull;
        const MemoryQualityConfig eight = MakeConfig(RenderingPath::Deferred, 1280u, 720u, 8u, kTechSSR);
        MemoryQualityConfig four = eight;
        four.MsaaSamples = 4u;

        ViewMemoryInventory inventory;
        inventory.DisplayWidth = 1280u;
        inventory.DisplayHeight = 720u;
        inventory.NodeTargetBytes = 10u * kMiB;
        inventory.TransientDemandBytes = 20u * kMiB;
        inventory.TransientSlackBytes = 3u * kMiB;
        inventory.TechniqueHistoryBytes[static_cast<u32>(MemoryTechnique::SSR)] = pixels * 8u;

        // Fewer samples: a whole new G-Buffer while the old one retires. Nothing else moves.
        const MemoryTransition fewerSamples = EstimateViewTransition(&eight, four, inventory);
        EXPECT_EQ(fewerSamples.NewBytes, pixels * texel * 5u);
        EXPECT_EQ(fewerSamples.ReleasedBytes, pixels * texel * 9u);
        EXPECT_TRUE(fewerSamples.Complete);

        // A technique fallback and the pool slack only release.
        MemoryQualityConfig noSsr = eight;
        noSsr.Techniques = 0u;
        noSsr.TransientPoolSpare = 0u;
        const MemoryTransition freeOnly = EstimateViewTransition(&eight, noSsr, inventory);
        EXPECT_EQ(freeOnly.NewBytes, 0u);
        EXPECT_EQ(freeOnly.ReleasedBytes, pixels * 8u + 3u * kMiB);

        // The same configuration costs nothing.
        const MemoryTransition same = EstimateViewTransition(&eight, eight, inventory);
        EXPECT_EQ(same.NewBytes, 0u);
        EXPECT_EQ(same.ReleasedBytes, 0u);
    }

    TEST(RendererMemoryFootprint, AResizeRescalesMeasuredOwnersAndChargesEverythingAnew)
    {
        const MemoryQualityConfig small = MakeConfig(RenderingPath::Deferred, 1280u, 720u, 1u, kTechSSR);
        const MemoryQualityConfig large = MakeConfig(RenderingPath::Deferred, 2560u, 1440u, 1u, kTechSSR);
        const u64 smallPixels = 1280ull * 720ull;

        ViewMemoryInventory inventory;
        inventory.DisplayWidth = 1280u;
        inventory.DisplayHeight = 720u;
        inventory.NodeTargetBytes = smallPixels * 24u;
        inventory.TransientDemandBytes = smallPixels * 68u;
        inventory.OtherHistoryBytes = smallPixels * 16u;
        inventory.TechniqueHistoryBytes[static_cast<u32>(MemoryTechnique::SSR)] = smallPixels * 8u;

        const MemoryTransition resize = EstimateViewTransition(&small, large, inventory);
        const u64 texel = GBufferTexelBytes();
        // Four times the pixels: every screen-sized owner is new at four times its size...
        EXPECT_EQ(resize.NewBytes, 4u * smallPixels * (texel + 8u + 24u + 68u + 16u));
        // ...and all of the old storage is still there until it retires.
        EXPECT_EQ(resize.ReleasedBytes, smallPixels * (texel + 8u + 24u + 68u + 16u));
        EXPECT_TRUE(resize.Complete);
    }

    TEST(RendererMemoryFootprint, AnUpscaleChangeAtTheSameDisplaySizeIsPricedByTheSceneBand)
    {
        // Performance upscale (half scale) switched off at an unchanged 1080p display.
        MemoryQualityConfig upscaled = MakeConfig(RenderingPath::Forward, 1920u, 1080u);
        upscaled.SceneWidth = 960u;
        upscaled.SceneHeight = 540u;
        const MemoryQualityConfig native = MakeConfig(RenderingPath::Forward, 1920u, 1080u);

        ViewMemoryInventory inventory;
        inventory.DisplayWidth = 1920u;
        inventory.DisplayHeight = 1080u;
        inventory.SceneWidth = 960u;
        inventory.SceneHeight = 540u;
        inventory.NodeTargetBytes = 40u * kMiB;
        inventory.TransientDemandBytes = 20u * kMiB;

        const MemoryTransition transition = EstimateViewTransition(&upscaled, native, inventory);
        // Four times the scene pixels: never priced as free, and never below the scene-band growth.
        EXPECT_EQ(transition.NewBytes, 4u * 60u * kMiB);
        EXPECT_EQ(transition.ReleasedBytes, 60u * kMiB);
    }

    TEST(RendererMemoryFootprint, AViewThatHasNotDrawnReportsAnIncompleteEstimateNotAZero)
    {
        const MemoryQualityConfig config = MakeConfig(RenderingPath::Deferred, 1280u, 720u, 4u, kTechSSR);
        const MemoryTransition first = EstimateViewTransition(nullptr, config, ViewMemoryInventory{});
        // The modelled owners are priced; the measured ones cannot be yet.
        EXPECT_EQ(first.NewBytes, EstimateGBufferBytes(config) + EstimateTechniqueHistoryBytes(MemoryTechnique::SSR, config));
        EXPECT_EQ(first.ReleasedBytes, 0u);
        EXPECT_FALSE(first.Complete) << "unmeasured pass targets and transients must not read as free";
    }

    TEST(RendererMemoryFootprint, ATechniqueThatIsOnButNotRunningReleasesWhatItMeasuredNotWhatTheModelSays)
    {
        // ReSTIR GI is on in the settings, but the registry holds nothing for it (no
        // ray-tracing backend): its fallback rung gives nothing back, and says so.
        const MemoryQualityConfig requested = MakeConfig(RenderingPath::Deferred, 1280u, 720u, 1u, kTechReSTIRGI);
        MemoryQualityConfig without = requested;
        without.Techniques = 0u;
        ViewMemoryInventory inventory;
        inventory.DisplayWidth = 1280u;
        inventory.DisplayHeight = 720u;
        const MemoryTransition transition = EstimateViewTransition(&requested, without, inventory);
        EXPECT_EQ(transition.ReleasedBytes, 0u);
        EXPECT_EQ(transition.NewBytes, 0u);
    }

    // ---------------------------------------------------------------------------------
    // Admission
    // ---------------------------------------------------------------------------------

    TEST(RendererMemoryGovernor, WithoutABudgetTheRequestIsActiveAndTheReasonSaysNothingWasEnforced)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.AddView(1, MakeConfig(RenderingPath::Deferred, 3840u, 2160u, 8u, kTechSSGI | kTechSSR));

        const auto steps = Drive(governor, renderer, std::nullopt, 6);
        for (const Step& step : steps)
            EXPECT_EQ(step, (Step{ 0u, MemoryAdmissionReason::BudgetUnknown, 0u }));
        const MemoryAdmissionSnapshot& snapshot = governor.GetSnapshot();
        EXPECT_EQ(snapshot.BudgetSource, MemoryBudgetSource::None);
        EXPECT_FALSE(snapshot.BudgetBytes.has_value());
        EXPECT_FALSE(snapshot.IsReduced());
        ASSERT_EQ(snapshot.Views.Num(), 1);
        EXPECT_FALSE(snapshot.Views[0].IsReduced());
    }

    TEST(RendererMemoryGovernor, AnAmpleBudgetAdmitsTheRequest)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.AddView(1, MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 8u, kTechSSGI | kTechSSR | kTechBloom));

        const auto steps = Drive(governor, renderer, 8192ull * kMiB, 40);
        for (const Step& step : steps)
            EXPECT_EQ(step, (Step{ 0u, MemoryAdmissionReason::Admitted, 0u }));
        EXPECT_EQ(governor.GetSnapshot().Transitions, 0u);
        EXPECT_EQ(governor.GetSnapshot().BudgetSource, MemoryBudgetSource::Injected);
    }

    TEST(RendererMemoryGovernor, PressureLandsOnTheFirstLevelThatFitsAndTwoReplaysAgree)
    {
        const auto replay = [](std::vector<Step>& out, MemoryAdmissionSnapshot& final, u64& liveAtEnd)
        {
            RendererMemoryGovernor governor;
            governor.SetPolicy(TestPolicy());
            SimulatedRenderer renderer;
            MemoryQualityConfig requested = MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 8u, kTechSSGI | kTechSSR | kTechBloom);
            requested.SSGIHalfResolution = false;
            renderer.AddView(1, requested);

            out = Drive(governor, renderer, 8192ull * kMiB, 10);
            // Pressure: a budget the requested configuration does not fit.
            const auto pressured = Drive(governor, renderer, 1400ull * kMiB, 80);
            out.insert(out.end(), pressured.begin(), pressured.end());
            final = governor.GetSnapshot();
            liveAtEnd = renderer.LiveBytes();
        };

        std::vector<Step> first;
        std::vector<Step> second;
        MemoryAdmissionSnapshot firstFinal;
        MemoryAdmissionSnapshot secondFinal;
        u64 firstLive = 0;
        u64 secondLive = 0;
        replay(first, firstFinal, firstLive);
        replay(second, secondFinal, secondLive);

        EXPECT_EQ(first, second) << "the same request under the same budget took a different path";
        EXPECT_EQ(firstLive, secondLive);

        // The budget buys 4x MSAA with the SSGI chain at half resolution: the first level
        // whose steady footprint fits. Nothing deeper is kept.
        const u64 limit = 1400ull * kMiB - 16ull * kMiB;
        EXPECT_LE(firstLive, limit);
        EXPECT_TRUE(firstFinal.IsReduced());
        ASSERT_EQ(firstFinal.Views.Num(), 1);
        EXPECT_EQ(firstFinal.Views[0].Requested.MsaaSamples, 8u) << "the request must survive the reduction";
        EXPECT_EQ(firstFinal.Views[0].Realized.MsaaSamples, 4u);
        EXPECT_TRUE(firstFinal.Views[0].Realized.SSGIHalfResolution);
        EXPECT_EQ(firstFinal.Views[0].Realized.Techniques, firstFinal.Views[0].Requested.Techniques)
            << "no technique was needed to fit; none may be taken";
        EXPECT_EQ(firstFinal.ActiveRungMask, RungBit("transientPoolSlack") | RungBit("historyResolution") | RungBit("msaa4"));
        EXPECT_EQ(firstFinal.Reason, MemoryAdmissionReason::DeviceBudget);
        // It got there and stayed: no level change after the pressure was answered.
        EXPECT_EQ(first.back().Level, firstFinal.Level);
        EXPECT_GT(firstFinal.TicksAtLevel, 20u);
    }

    TEST(RendererMemoryGovernor, ADescentThatAllocatesIsStagedSoItsOverlapStaysSmall)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        MemoryQualityConfig requested = MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 8u, kTechSSGI | kTechSSR | kTechBloom);
        requested.SSGIHalfResolution = false;
        renderer.AddView(1, requested);
        (void)Drive(governor, renderer, 8192ull * kMiB, 10);

        const u64 residentBefore = renderer.ResidentBytes();
        const u64 budget = 1400ull * kMiB;
        ASSERT_GT(residentBefore, budget) << "the scenario needs the device to be over the new budget already";
        renderer.ResetPeak();

        // The frame the pressure is answered: the 4x G-Buffer would fit once the 8x one
        // has retired, but not beside it. The governor goes through 1x first.
        const MemoryAdmissionSnapshot staged = governor.Tick(renderer.Input(budget));
        ASSERT_EQ(staged.Views.Num(), 1);
        EXPECT_EQ(staged.Views[0].Realized.MsaaSamples, 1u) << "the direct 8x -> 4x replacement was taken despite its overlap";
        EXPECT_EQ(staged.Reason, MemoryAdmissionReason::DeviceBudget);
        const u64 predictedPeak = staged.PredictedPeakBytes;
        renderer.Render(staged);

        const u64 directOverlap = EstimateGBufferBytes(ReduceMemoryQuality(requested, 3u));
        EXPECT_LT(renderer.PeakResident() - residentBefore, directOverlap / 2u)
            << "the staged descent allocated as much as the direct one would have";
        EXPECT_LE(renderer.PeakResident(), predictedPeak) << "resident bytes went past what the governor predicted";

        // Once the old storage is gone it climbs to the level the budget really buys, and
        // never past it.
        (void)Drive(governor, renderer, budget, 80);
        const MemoryAdmissionSnapshot& settled = governor.GetSnapshot();
        EXPECT_EQ(settled.Views[0].Realized.MsaaSamples, 4u);
        EXPECT_LE(renderer.LiveBytes(), budget - 16ull * kMiB);
        EXPECT_LE(renderer.PeakResident(), std::max(residentBefore + directOverlap / 2u, budget))
            << "a later transition raised the peak past the budget";
        EXPECT_EQ(settled.FailedRecoveries, 0u);
    }

    TEST(RendererMemoryGovernor, RecoveryWaitsOutAStableRunAndAShortReliefChangesNothing)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.AddView(1, MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 1u, kTechSSGI | kTechSSR | kTechReSTIRGI));

        const u64 low = 800ull * kMiB;
        const u64 high = 8192ull * kMiB;
        (void)Drive(governor, renderer, high, 10);
        (void)Drive(governor, renderer, low, 30);
        const MemoryAdmissionSnapshot pressured = governor.GetSnapshot();
        ASSERT_TRUE(pressured.IsReduced());
        const u64 transitionsUnderPressure = pressured.Transitions;

        // Relief shorter than the recovery run, six times over: the level never moves.
        for (u32 pulse = 0; pulse < 6; ++pulse)
        {
            const auto relief = Drive(governor, renderer, high, TestPolicy().RecoveryFrames - 2u);
            for (const Step& step : relief)
            {
                EXPECT_EQ(step.Level, pressured.Level);
                EXPECT_EQ(step.Reason, MemoryAdmissionReason::RecoveryHold) << "headroom is back and must be reported as being timed";
            }
            (void)Drive(governor, renderer, low, 3);
        }
        EXPECT_EQ(governor.GetSnapshot().Transitions, transitionsUnderPressure) << "a pulse of headroom caused an allocation";
        EXPECT_EQ(governor.GetSnapshot().Level, pressured.Level);

        // Sustained relief: exactly one climb, straight to the request.
        const auto recovered = Drive(governor, renderer, high, 40);
        EXPECT_EQ(recovered.back(), (Step{ 0u, MemoryAdmissionReason::Admitted, 0u }));
        EXPECT_EQ(governor.GetSnapshot().Recoveries, 1u);
        EXPECT_EQ(governor.GetSnapshot().Transitions, transitionsUnderPressure + 1u);
        EXPECT_EQ(governor.GetSnapshot().FailedRecoveries, 0u);
        ASSERT_EQ(governor.GetSnapshot().Views.Num(), 1);
        EXPECT_FALSE(governor.GetSnapshot().Views[0].IsReduced());
    }

    TEST(RendererMemoryGovernor, TheFloorIsKeptAndReportedWhenEvenItDoesNotFit)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        const MemoryQualityConfig requested = MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 8u, kTechSSGI | kTechSSR | kTechBloom);
        renderer.AddView(1, requested);
        (void)Drive(governor, renderer, 8192ull * kMiB, 10);

        // Less than the base alone: nothing can fit.
        const auto steps = Drive(governor, renderer, 256ull * kMiB, 60);
        const MemoryAdmissionSnapshot& snapshot = governor.GetSnapshot();
        EXPECT_EQ(snapshot.Level, snapshot.FloorLevel);
        EXPECT_EQ(snapshot.Reason, MemoryAdmissionReason::OverBudgetAtFloor);
        ASSERT_EQ(snapshot.Views.Num(), 1);
        const MemoryQualityConfig& realized = snapshot.Views[0].Realized;
        // The minimum complete image: the path and both resolutions are untouched.
        EXPECT_EQ(realized.Path, requested.Path);
        EXPECT_EQ(realized.DisplayWidth, requested.DisplayWidth);
        EXPECT_EQ(realized.DisplayHeight, requested.DisplayHeight);
        EXPECT_EQ(realized.SceneWidth, requested.SceneWidth);
        EXPECT_EQ(realized.MsaaSamples, 1u);
        EXPECT_EQ(realized.Techniques, 0u);
        // It reached the floor in a bounded number of moves and then stopped moving.
        EXPECT_LE(snapshot.Degradations, 2u);
        EXPECT_EQ(steps.back().Level, steps[steps.size() - 20u].Level);
        EXPECT_EQ(snapshot.Recoveries, 0u);
    }

    TEST(RendererMemoryGovernor, AResizeIsAdmittedBeforeItsTargetsExistAndRecoversOnceTheOldOnesRetire)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.AddView(1, MakeConfig(RenderingPath::Deferred, 1280u, 720u, 1u, kTechReSTIRGI));
        const u64 budget = 1400ull * kMiB;
        (void)Drive(governor, renderer, budget, 12);
        ASSERT_EQ(governor.GetSnapshot().Reason, MemoryAdmissionReason::Admitted);

        // The window grows. The larger configuration fits the budget on its own, but not
        // while the smaller one's targets are still resident.
        MemoryQualityConfig& requested = renderer.Requested(1);
        requested.DisplayWidth = requested.SceneWidth = 2560u;
        requested.DisplayHeight = requested.SceneHeight = 1440u;
        const u64 steadyLarge = renderer.BaseBytes + renderer.Footprint(requested);
        ASSERT_LE(steadyLarge, budget - 16ull * kMiB);
        ASSERT_GT(renderer.ResidentBytes() + renderer.Footprint(requested), budget);

        renderer.ResetPeak();
        const MemoryAdmissionSnapshot admitted = governor.Readmit(renderer.Input(budget));
        EXPECT_EQ(admitted.Reason, MemoryAdmissionReason::TransitionOverlap);
        ASSERT_EQ(admitted.Views.Num(), 1);
        EXPECT_EQ(admitted.Views[0].Realized.DisplayWidth, 2560u) << "the realized size is the requested one; only quality is reduced";
        EXPECT_FALSE(admitted.Views[0].Realized.Has(MemoryTechnique::ReSTIRGI));
        EXPECT_EQ(admitted.Readmissions, 1u);
        EXPECT_LE(admitted.PredictedPeakBytes, budget);
        renderer.Render(admitted);
        EXPECT_LE(renderer.PeakResident(), budget) << "the resize evaded the budget while its old targets were in flight";

        // The old targets retire; the technique comes back after a stable run.
        const auto steps = Drive(governor, renderer, budget, 40);
        EXPECT_EQ(steps.back(), (Step{ 0u, MemoryAdmissionReason::Admitted, 0u }));
        EXPECT_LE(renderer.PeakResident(), budget);
        EXPECT_EQ(governor.GetSnapshot().Recoveries, 1u);
    }

    TEST(RendererMemoryGovernor, AnUnmodelledSavingIsMeasuredOnTheWayDownSoItsClimbIsNeverTried)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        // Bloom holds transients the model cannot price: 200 MiB of them here.
        renderer.HiddenBytesPerTechnique = 200ull * kMiB;
        renderer.AddView(1, MakeConfig(RenderingPath::Forward, 1920u, 1080u, 1u, kTechBloom));
        const u64 budget = 850ull * kMiB;

        // No level is PREDICTED to fit, so the governor steps and measures: the slack
        // rung first, then bloom, which turns out to be what was needed.
        (void)Drive(governor, renderer, budget, 12);
        ASSERT_FALSE(governor.GetSnapshot().Views[0].Realized.Has(MemoryTechnique::Bloom));
        ASSERT_LE(renderer.LiveBytes(), budget - 16ull * kMiB);
        EXPECT_EQ(governor.GetSnapshot().Degradations, 2u);

        // The model says climbing back is free. The descent measured that it is not, so
        // the climb is never tried: no reallocation, no second descent.
        (void)Drive(governor, renderer, budget, 400);
        const MemoryAdmissionSnapshot& snapshot = governor.GetSnapshot();
        EXPECT_EQ(snapshot.Recoveries, 0u) << "a climb the descent had already measured as too expensive was tried";
        EXPECT_EQ(snapshot.FailedRecoveries, 0u);
        EXPECT_EQ(snapshot.Degradations, 2u);
        EXPECT_FALSE(snapshot.Views[0].Realized.Has(MemoryTechnique::Bloom));
        EXPECT_GT(snapshot.TicksAtLevel, 300u);

        // A larger budget is new information: the climb is taken, and holds.
        (void)Drive(governor, renderer, 4096ull * kMiB, 60);
        EXPECT_FALSE(governor.GetSnapshot().IsReduced());
        EXPECT_EQ(governor.GetSnapshot().FailedRecoveries, 0u);
    }

    TEST(RendererMemoryGovernor, AnOsBudgetThatDriftsDoesNotForgetWhatADescentTaught)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.HiddenBytesPerTechnique = 200ull * kMiB;
        renderer.AddView(1, MakeConfig(RenderingPath::Forward, 1920u, 1080u, 1u, kTechBloom));
        const u64 budget = 850ull * kMiB;
        (void)Drive(governor, renderer, budget, 12);

        // The budget wobbles by a megabyte a frame, as an OS-reported one does.
        for (u32 frame = 0; frame < 400; ++frame)
        {
            const u64 wobble = (frame % 3u) * kMiB;
            const MemoryAdmissionSnapshot& decision = governor.Tick(renderer.Input(budget + wobble));
            renderer.Render(decision);
        }
        EXPECT_EQ(governor.GetSnapshot().Recoveries, 0u) << "budget noise reset what the descent had measured";
        EXPECT_EQ(governor.GetSnapshot().FailedRecoveries, 0u);
    }

    TEST(RendererMemoryGovernor, PressureRightAfterAClimbUndoesItAndDoublesTheWait)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.AddView(1, MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 1u, kTechReSTIRGI));

        (void)Drive(governor, renderer, 850ull * kMiB, 30);
        const MemoryAdmissionSnapshot pressured = governor.GetSnapshot();
        ASSERT_FALSE(pressured.Views[0].Realized.Has(MemoryTechnique::ReSTIRGI));

        // Relief: the technique fits again, and comes back after a stable run.
        const u64 relieved = 1100ull * kMiB;
        (void)Drive(governor, renderer, relieved, TestPolicy().RecoveryFrames + TestPolicy().SettleFrames + 1u);
        ASSERT_TRUE(governor.GetSnapshot().Views[0].Realized.Has(MemoryTechnique::ReSTIRGI));
        ASSERT_EQ(governor.GetSnapshot().Recoveries, 1u);

        // Something else grows right after it (a streamed asset): the climb no longer fits.
        renderer.BaseBytes += 200ull * kMiB;
        (void)Drive(governor, renderer, relieved, 200);
        const MemoryAdmissionSnapshot& snapshot = governor.GetSnapshot();
        EXPECT_EQ(snapshot.FailedRecoveries, 1u);
        EXPECT_EQ(snapshot.Level, pressured.Level) << "the undone climb did not return to the level it left";
        EXPECT_FALSE(snapshot.Views[0].Realized.Has(MemoryTechnique::ReSTIRGI));
        EXPECT_EQ(snapshot.RecoveryRequiredFrames, TestPolicy().RecoveryFrames * 2u) << "the wait did not double";
        EXPECT_EQ(snapshot.Recoveries, 1u) << "the climb was retried into the same pressure";
        EXPECT_LE(renderer.LiveBytes(), relieved - 16ull * kMiB);
    }

    TEST(RendererMemoryGovernor, TwoViewsShareOneBudget)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        const MemoryQualityConfig config = MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 4u, kTechSSR);
        renderer.AddView(1, config);

        // One view fits this budget as requested.
        const u64 budget = 1536ull * kMiB;
        (void)Drive(governor, renderer, budget, 12);
        ASSERT_EQ(governor.GetSnapshot().Reason, MemoryAdmissionReason::Admitted);

        // A second view of the same configuration does not fit beside it. The fresh
        // admission reduces BOTH, by the same rungs, before the second one allocates, and
        // prices the second view's pass targets from the first one's measurement. 2x
        // would fit once the first view's 4x G-Buffer has retired, but not beside it, so
        // both go to 1x.
        renderer.AddView(2, config);
        renderer.ResetPeak();
        (void)Drive(governor, renderer, budget, 30);
        const MemoryAdmissionSnapshot twoViews = governor.GetSnapshot();
        ASSERT_EQ(twoViews.Views.Num(), 2);
        EXPECT_TRUE(twoViews.IsReduced());
        EXPECT_EQ(twoViews.Views[0].Realized, twoViews.Views[1].Realized) << "one ladder level applies to every view";
        EXPECT_EQ(twoViews.Views[0].Realized.MsaaSamples, 1u);
        EXPECT_TRUE(twoViews.EstimateComplete) << "the view that had not drawn was priced without its pass targets";
        EXPECT_LE(renderer.LiveBytes(), budget - 16ull * kMiB);
        EXPECT_LE(renderer.PeakResident(), budget) << "the second view's targets were allocated past the budget";

        // The second view goes away: the first returns to its request.
        renderer.RemoveView(2);
        (void)Drive(governor, renderer, budget, 40);
        const MemoryAdmissionSnapshot& oneView = governor.GetSnapshot();
        ASSERT_EQ(oneView.Views.Num(), 1);
        EXPECT_FALSE(oneView.IsReduced());
        EXPECT_EQ(oneView.Views[0].Realized.MsaaSamples, 4u);
    }

    TEST(RendererMemoryGovernor, OptionalDetailIsGivenBackBeforeAnyTechniqueAndReturns)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.OptionalDetailBytes = 300ull * kMiB;
        MemoryQualityConfig requested = MakeConfig(RenderingPath::Forward, 1920u, 1080u, 1u, kTechSSR | kTechBloom);
        requested.OptionalDetail = true;
        renderer.AddView(1, requested);
        (void)Drive(governor, renderer, 8192ull * kMiB, 10);

        // The budget is short by less than the optional detail: the pinned floor of the
        // groom / vegetation representations is enough, and every technique stays.
        const u64 budget = 800ull * kMiB;
        (void)Drive(governor, renderer, budget, 30);
        const MemoryAdmissionSnapshot pressured = governor.GetSnapshot();
        ASSERT_EQ(pressured.Views.Num(), 1);
        EXPECT_FALSE(pressured.Views[0].Realized.OptionalDetail);
        EXPECT_EQ(pressured.Views[0].Realized.Techniques, requested.Techniques);
        EXPECT_EQ(pressured.ActiveRungMask, RungBit("transientPoolSlack") | RungBit("optionalDetailPools"));
        EXPECT_EQ(pressured.Degradations, 1u) << "the saving is known; it must be taken in one move";
        EXPECT_LE(renderer.LiveBytes(), budget - 16ull * kMiB);

        (void)Drive(governor, renderer, 8192ull * kMiB, 30);
        EXPECT_TRUE(governor.GetSnapshot().Views[0].Realized.OptionalDetail);
        EXPECT_FALSE(governor.GetSnapshot().IsReduced());
    }

    TEST(RendererMemoryGovernor, AChangedRequestUnderTheSameBudgetKeepsWhatWasMeasured)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.HiddenBytesPerTechnique = 200ull * kMiB;
        renderer.AddView(1, MakeConfig(RenderingPath::Forward, 1920u, 1080u, 1u, kTechBloom));
        const u64 budget = 850ull * kMiB;
        (void)Drive(governor, renderer, budget, 12);
        ASSERT_FALSE(governor.GetSnapshot().Views[0].Realized.Has(MemoryTechnique::Bloom));

        // The viewport is dragged a few pixels at a time: every frame is a readmission.
        for (u32 step = 0; step < 20; ++step)
        {
            MemoryQualityConfig& requested = renderer.Requested(1);
            requested.DisplayWidth = requested.SceneWidth = 1920u - 8u * (step + 1u);
            (void)Drive(governor, renderer, budget, 1);
            EXPECT_FALSE(governor.GetSnapshot().Views[0].Realized.Has(MemoryTechnique::Bloom))
                << "a readmission at step " << step << " re-enabled the technique the descent measured to be too expensive";
        }
        EXPECT_EQ(governor.GetSnapshot().Readmissions, 20u);
        EXPECT_LE(renderer.LiveBytes(), budget - 16ull * kMiB);
    }

    TEST(RendererMemoryGovernor, AClimbThatHeldIsNotChargedAgainForTheRungsItPassed)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        // Two techniques the model cannot price, 200 MiB each.
        renderer.HiddenBytesPerTechnique = 200ull * kMiB;
        const u32 techniques = kTechBloom | MemoryTechniqueBit(MemoryTechnique::DepthOfField);
        renderer.AddView(1, MakeConfig(RenderingPath::Forward, 1920u, 1080u, 1u, techniques));
        (void)Drive(governor, renderer, 8192ull * kMiB, 10);
        const u64 full = renderer.LiveBytes();

        // Room for one of the two only: both go, one at a time, each measured on the way down.
        (void)Drive(governor, renderer, full - 250ull * kMiB, 40);
        ASSERT_EQ(governor.GetSnapshot().Views[0].Realized.Techniques & techniques, 0u);

        // Room for one again with the recovery band to spare: the rung removed last
        // (bloom) is the first one a climb restores, and it pays only bloom's measured cost.
        (void)Drive(governor, renderer, full - 100ull * kMiB, 60);
        const MemoryQualityConfig partial = governor.GetSnapshot().Views[0].Realized;
        EXPECT_TRUE(partial.Has(MemoryTechnique::Bloom)) << "the climb was charged for depth of field too";
        EXPECT_FALSE(partial.Has(MemoryTechnique::DepthOfField));
        EXPECT_EQ(governor.GetSnapshot().Recoveries, 1u);

        // Room for both: the last climb pays only for bloom, and is taken.
        (void)Drive(governor, renderer, full + 100ull * kMiB, 60);
        EXPECT_FALSE(governor.GetSnapshot().IsReduced()) << "a held climb's cost was charged again to the next one";
        EXPECT_EQ(governor.GetSnapshot().FailedRecoveries, 0u);
    }

    TEST(RendererMemoryGovernor, ABudgetThatBecomesUnknownIsNotCountedAsARecovery)
    {
        RendererMemoryGovernor governor;
        governor.SetPolicy(TestPolicy());
        SimulatedRenderer renderer;
        renderer.AddView(1, MakeConfig(RenderingPath::Deferred, 1920u, 1080u, 4u, kTechSSR));
        (void)Drive(governor, renderer, 700ull * kMiB, 30);
        ASSERT_TRUE(governor.GetSnapshot().IsReduced());
        const MemoryAdmissionSnapshot before = governor.GetSnapshot();

        (void)Drive(governor, renderer, std::nullopt, 5);
        const MemoryAdmissionSnapshot& after = governor.GetSnapshot();
        EXPECT_EQ(after.Reason, MemoryAdmissionReason::BudgetUnknown);
        EXPECT_FALSE(after.IsReduced());
        EXPECT_EQ(after.Recoveries, before.Recoveries) << "nothing was measured to fit; this is not a recovery";
        EXPECT_EQ(after.Transitions, before.Transitions);
    }

    TEST(RendererMemoryGovernor, ReasonCodesHaveStableSpellings)
    {
        EXPECT_STREQ(ToString(MemoryAdmissionReason::Admitted), "admitted");
        EXPECT_STREQ(ToString(MemoryAdmissionReason::BudgetUnknown), "budgetUnknown");
        EXPECT_STREQ(ToString(MemoryAdmissionReason::DeviceBudget), "deviceBudget");
        EXPECT_STREQ(ToString(MemoryAdmissionReason::TransitionOverlap), "transitionOverlap");
        EXPECT_STREQ(ToString(MemoryAdmissionReason::RecoveryHold), "recoveryHold");
        EXPECT_STREQ(ToString(MemoryAdmissionReason::OverBudgetAtFloor), "overBudgetAtFloor");
        EXPECT_STREQ(ToString(MemoryBudgetSource::None), "none");
        EXPECT_STREQ(ToString(MemoryBudgetSource::OsReported), "osReported");
        EXPECT_STREQ(ToString(MemoryBudgetSource::AllocatorHeuristic), "allocatorHeuristic");
        EXPECT_STREQ(ToString(MemoryBudgetSource::Configured), "configured");
        EXPECT_STREQ(ToString(MemoryBudgetSource::Injected), "injected");
    }
} // namespace OloEngine::Tests
