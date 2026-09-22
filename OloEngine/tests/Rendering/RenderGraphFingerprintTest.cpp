#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"

#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/FrameGraphDeclarationConfig.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/DepthVelocityUpscalePass.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/Passes/OITPrepareRenderPass.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/Passes/ReSTIRDIPass.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// =============================================================================
// Render-graph declaration-key tests (issue #1333; the SSR bug before it).
//
// WHY THIS EXISTS
// ---------------
// PopulateBlackboard and RenderGraph::BuildFrameGraph both skip their work
// while the declaration key matches the previous frame's, and BuildFrameGraph
// does not re-run a single pass's Setup() on a hit. So ANY input that changes
// what is declared must move the key, or the change is silently ignored: the
// effect's resource is never declared, its pass is culled, and the effect
// no-ops. The SSR enable, the IBL identities, precipitation, the GTAO denoise
// gate, the display size under FSR1 and four bucket-gated stream passes all
// shipped missing from the old hand-written fingerprint.
//
// The key is now derived, not assembled: FrameGraphDeclarationConfig's fields
// generate it, and every pass the pipeline owns contributes its own inputs
// through RenderGraphNode::AppendDeclarationInputs. These tests pin both
// halves, in both directions, because the failure goes both ways:
//   * a declaration input that does NOT move the key freezes a stale graph;
//   * an execution-only input that DOES move it rebuilds the graph every frame
//     it changes, which is a camera move away from rebuilding every frame.
//
// Pure CPU: a default Renderer3DData builds its own RenderPipeline with no
// passes and no graph, and every pass constructed here is CPU-only.
//
// Classification: plumbing (render-graph cache-invalidation contract, no GL).
// =============================================================================

namespace OloEngine::Tests
{
    // Friended by Renderer3D so the test can reach the private RenderPipeline /
    // Renderer3DData and call the otherwise-internal key hooks.
    struct RenderPipelineFingerprintAccess
    {
        // Public spelling of the private type, for the test bodies below.
        using Data = Renderer3D::Renderer3DData;

        [[nodiscard]] static u64 Fingerprint(const PostProcessSettings& pp, const RendererSettings& rs)
        {
            Renderer3D::Renderer3DData data; // ctor makes its own RenderPipeline; no GL
            data.PostProcess = pp;
            data.Settings = rs;
            return data.Pipeline->ComputeDeclarationKey(data);
        }

        // Runs `mutate(data)` between two key computations on ONE
        // Renderer3DData, so everything the mutation does not touch is
        // identical by construction. Returns {before, after}.
        template<typename Mutate>
        [[nodiscard]] static std::pair<u64, u64> KeyAcross(Mutate&& mutate, bool withGraph = false)
        {
            Renderer3D::Renderer3DData data;
            data.Settings.Path = RenderingPath::Deferred;
            if (withGraph)
            {
                data.RGraph = Ref<RenderGraph>::Create();
                data.RGraph->Init(1280u, 720u);
            }
            const u64 before = data.Pipeline->ComputeDeclarationKey(data);
            mutate(data);
            const u64 after = data.Pipeline->ComputeDeclarationKey(data);
            return { before, after };
        }

        [[nodiscard]] static FrameGraphDeclarationConfig Capture(const Renderer3D::Renderer3DData& data)
        {
            return data.Pipeline->CaptureDeclarationConfig(data);
        }

        // The IDENTITIES the blackboard imports (as opposed to declares).
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
            return data.Pipeline->ComputeDeclarationKey(data);
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
        // instance, with a draw submitted in between: the pass outlives the
        // frame and its bucket fills, which is what actually happens.
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
            out.Empty = data.Pipeline->ComputeDeclarationKey(data);

            // The allocator outlives both remaining calls; the packet's
            // contents are never read, only counted.
            CommandAllocator allocator;
            ClearCommand payload{};
            node->GetCommandBucket().Submit(payload, {}, &allocator);

            out.WithDraw = data.Pipeline->ComputeDeclarationKey(data);
            out.Restated = data.Pipeline->ComputeDeclarationKey(data);
            return out;
        }

        // How many passes ForEachPass visits, and how many members the four
        // pass sets hold between them.
        [[nodiscard]] static std::pair<u32, u32> PassWalkCoverage()
        {
            Renderer3D::Renderer3DData data;
            u32 visited = 0;
            data.Pipeline->ForEachPass([&visited](const auto&)
                                       { ++visited; });
            const u32 members =
                static_cast<u32>(std::tuple_size_v<decltype(Renderer3D::FrameCorePassSet::Members())> +
                                 std::tuple_size_v<decltype(Renderer3D::SceneCompositionPassSet::Members())> +
                                 std::tuple_size_v<decltype(Renderer3D::RenderStreamPassSet::Members())> +
                                 std::tuple_size_v<decltype(Renderer3D::PostProcessPassChain::Members())>);
            return { visited, members };
        }
    };

    namespace
    {
        using Access = RenderPipelineFingerprintAccess;

        [[nodiscard]] RendererSettings DeferredSettings()
        {
            RendererSettings rs;
            rs.Path = RenderingPath::Deferred;
            return rs;
        }

        // The smallest change to a field of each type the configuration holds.
        template<typename T>
        [[nodiscard]] T Nudged(T value)
        {
            if constexpr (std::is_same_v<T, bool>)
                return !value;
            else if constexpr (std::is_enum_v<T>)
                return static_cast<T>(std::to_underlying(value) + 1);
            else if constexpr (std::is_integral_v<T>)
                return static_cast<T>(value + 1u);
            else
            {
                // An identity: same slot, next generation. That is the shape of
                // a destroyed resource whose slot was reissued to its
                // replacement, the case a name-keyed import cannot see.
                value.Generation += 1u;
                return value;
            }
        }

        struct Toggle
        {
            const char* Name;
            void (*Apply)(Access::Data&);
        };
    } // namespace

    // -------------------------------------------------------------------------
    // The configuration itself: every field is in the key, by construction.
    // -------------------------------------------------------------------------

    // AC1, structurally: for EACH declared configuration field, a mutation
    // moves the key and the diff names exactly that field. Generated from the
    // field table, so a field added to OLO_FRAME_GRAPH_DECLARATION_FIELDS is
    // tested without anyone adding a line here.
    TEST(FrameGraphDeclarationConfig, EveryFieldMovesTheKeyAndIsNamedInTheDiff)
    {
        const FrameGraphDeclarationConfig base{};
        const u64 baseKey = base.ComputeKey();
        u32 visited = 0;

        FrameGraphDeclarationConfig::ForEachField(
            [&](std::string_view name, auto member)
            {
                ++visited;
                FrameGraphDeclarationConfig changed = base;
                changed.*member = Nudged(changed.*member);
                EXPECT_NE(changed.ComputeKey(), baseKey)
                    << "Changing '" << name << "' did not move the declaration key. Every field of "
                                               "FrameGraphDeclarationConfig is a declaration input by definition; if this one is not, "
                                               "remove it from the table rather than from the key.";
                EXPECT_EQ(changed.DescribeDifferences(base), std::string(name));
                EXPECT_FALSE(changed == base);
            });

        EXPECT_EQ(visited, FrameGraphDeclarationConfig::kFieldCount);
        EXPECT_EQ(base.DescribeDifferences(base), "") << "An unchanged configuration must describe no change.";
    }

    // The captured PassStates field is a walk over every pass the pipeline
    // owns. If ForEachPass skipped a set, a pass in it could gate Setup() on
    // anything and never move the key: the #1315 class, one level up.
    TEST(FrameGraphDeclarationConfig, PassWalkCoversEveryPipelinePass)
    {
        const auto [visited, members] = Access::PassWalkCoverage();
        EXPECT_EQ(visited, members);
        EXPECT_GE(visited, 60u) << "The pipeline owns 60 passes today; a much smaller walk means a set went missing.";
    }

    // -------------------------------------------------------------------------
    // Pipeline-level inputs PopulateBlackboard gates declarations on.
    // -------------------------------------------------------------------------

    TEST(RenderGraphFingerprint, EveryPopulateGateMovesTheKey)
    {
        const std::vector<Toggle> toggles = {
            { "SSAOEnabled", [](Access::Data& d)
              { d.PostProcess.SSAOEnabled = !d.PostProcess.SSAOEnabled; } },
            { "GTAOEnabled", [](Access::Data& d)
              { d.PostProcess.GTAOEnabled = !d.PostProcess.GTAOEnabled; } },
            // Issue #708: half resolution sizes every resource in the SSGI
            // denoiser chain AND its four temporal histories.
            { "SSGIHalfResolution", [](Access::Data& d)
              { d.PostProcess.SSGIHalfResolution = !d.PostProcess.SSGIHalfResolution; } },
            { "DOFEnabled", [](Access::Data& d)
              { d.PostProcess.DOFEnabled = !d.PostProcess.DOFEnabled; } },
            { "MotionBlurEnabled", [](Access::Data& d)
              { d.PostProcess.MotionBlurEnabled = !d.PostProcess.MotionBlurEnabled; } },
            { "TAAEnabled", [](Access::Data& d)
              { d.PostProcess.TAAEnabled = !d.PostProcess.TAAEnabled; } },
            { "CASEnabled", [](Access::Data& d)
              { d.PostProcess.CASEnabled = !d.PostProcess.CASEnabled; } },
            { "ChromaticAberrationEnabled", [](Access::Data& d)
              { d.PostProcess.ChromaticAberrationEnabled = !d.PostProcess.ChromaticAberrationEnabled; } },
            { "ColorGradingEnabled", [](Access::Data& d)
              { d.PostProcess.ColorGradingEnabled = !d.PostProcess.ColorGradingEnabled; } },
            { "VignetteEnabled", [](Access::Data& d)
              { d.PostProcess.VignetteEnabled = !d.PostProcess.VignetteEnabled; } },
            { "FXAAEnabled", [](Access::Data& d)
              { d.PostProcess.FXAAEnabled = !d.PostProcess.FXAAEnabled; } },
            { "OverdrawDebugView", [](Access::Data& d)
              { d.PostProcess.OverdrawDebugView = !d.PostProcess.OverdrawDebugView; } },
            { "Upscale", [](Access::Data& d)
              { d.PostProcess.Upscale = UpscaleMode::Performance; } },
            { "Fog.Enabled", [](Access::Data& d)
              { d.Fog.Enabled = !d.Fog.Enabled; } },
            { "Cloudscape.Enabled", [](Access::Data& d)
              { d.Cloudscape.Enabled = !d.Cloudscape.Enabled; } },
            // #1333, U2: missing from the old fingerprint entirely.
            { "Precipitation screen effects", [](Access::Data& d)
              {
                  d.Precipitation.Enabled = true;
                  d.Precipitation.ScreenStreaksEnabled = true;
              } },
            { "Snow subsurface blur", [](Access::Data& d)
              {
                  d.Snow.Enabled = true;
                  d.Snow.SSSBlurEnabled = true;
              } },
            { "OITEnabled", [](Access::Data& d)
              { d.Settings.OITEnabled = !d.Settings.OITEnabled; } },
            { "Path", [](Access::Data& d)
              { d.Settings.Path = RenderingPath::Forward; } },
            { "ActiveGraphAOTechnique", [](Access::Data& d)
              { d.ActiveGraphAOTechnique = AOTechnique::GTAO; } },
        };

        for (const Toggle& t : toggles)
        {
            // Start every toggle from all-off so an enable/disable pair is not
            // cancelled by a default that already had it on.
            const auto [before, after] = Access::KeyAcross(
                [&t](Access::Data& d)
                { t.Apply(d); });
            EXPECT_NE(before, after)
                << "Changing '" << t.Name << "' did not move the declaration key, but PopulateBlackboard "
                                             "gates a declaration on it. Capture it into FrameGraphDeclarationConfig, or the graph "
                                             "won't repopulate when it changes and the effect silently no-ops.";
        }
    }

    // #1333, U1: the post chain is sized from the graph's DISPLAY size, the lit
    // scene from the scene band, and under FSR1 the two move independently: a
    // display resize that floors to the same band left every display-sized
    // target at the old size, because only the band was hashed.
    TEST(RenderGraphFingerprint, DisplaySizeMovesTheKeyWhenTheSceneBandDoesNot)
    {
        const auto [before, after] = Access::KeyAcross(
            [](Access::Data& d)
            { d.RGraph->Init(1281u, 720u); },
            /*withGraph=*/true);
        EXPECT_NE(before, after) << "A display resize must move the key even when the scene band is unchanged.";
    }

    // ResetTopology wipes the blackboard; the key must see that even when every
    // other input is identical (#530). Folded in as a configuration field.
    TEST(RenderGraphFingerprint, TopologyResetMovesTheKey)
    {
        const auto [before, after] = Access::KeyAcross(
            [](Access::Data& d)
            { d.RGraph->ResetTopology(); },
            /*withGraph=*/true);
        EXPECT_NE(before, after);
    }

    // -------------------------------------------------------------------------
    // Per-pass inputs, contributed through AppendDeclarationInputs.
    // -------------------------------------------------------------------------

    // Every pass the pipeline owns reports its enable, whichever set it lives
    // in. EASU and DepthVelocityUpscale gate PopulateBlackboard declarations on
    // their readiness and were missing from the old hand-written list (#1333,
    // U4); Precipitation's enable was missing too (U2).
    TEST(RenderGraphFingerprint, APassEnableMovesTheKeyInEverySet)
    {
        struct Case
        {
            const char* Name;
            void (*Install)(Access::Data&);
            void (*Flip)(Access::Data&);
        };
        const std::vector<Case> cases = {
            { "EASURenderPass (post chain)",
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.EASU = Ref<EASURenderPass>::Create(); },
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.EASU->SetEnabled(true); } },
            { "DepthVelocityUpscalePass (post chain)",
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.DepthVelocityUpscale = Ref<DepthVelocityUpscalePass>::Create(); },
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.DepthVelocityUpscale->SetEnabled(true); } },
            { "PrecipitationRenderPass (post chain)",
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.Precipitation = Ref<PrecipitationRenderPass>::Create(); },
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.Precipitation->SetEnabled(true); } },
            { "BloomRenderPass (post chain)",
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.Bloom = Ref<BloomRenderPass>::Create(); },
              [](Access::Data& d)
              { d.Pipeline->PostProcessPasses.Bloom->SetEnabled(true); } },
            { "OITPrepareRenderPass (scene composition)",
              [](Access::Data& d)
              { d.Pipeline->SceneCompositePasses.OITPrepare = Ref<OITPrepareRenderPass>::Create(); },
              [](Access::Data& d)
              { d.Pipeline->SceneCompositePasses.OITPrepare->SetEnabled(true); } },
        };

        for (const Case& c : cases)
        {
            {
                const auto [before, after] = Access::KeyAcross(c.Install);
                EXPECT_NE(before, after) << c.Name << ": installing the pass must move the key.";
            }
            Access::Data data;
            c.Install(data);
            const u64 disabled = data.Pipeline->ComputeDeclarationKey(data);
            c.Flip(data);
            EXPECT_NE(data.Pipeline->ComputeDeclarationKey(data), disabled)
                << c.Name << ": the pass's IsEnabled() gates what it declares, so it must move the key.";
        }
    }

    // #1333, U3: the GTAO denoise chain declares its pong target only when it
    // runs. Turning denoise on against a warm graph left Execute without the
    // pong, and it published "no occlusion" every frame.
    TEST(RenderGraphFingerprint, GTAODenoiseGateMovesTheKeyButItsPassCountParityDoesNotMatter)
    {
        Access::Data data;
        auto gtao = Ref<GTAORenderPass>::Create();
        data.Pipeline->SceneCompositePasses.GTAO = gtao;

        PostProcessSettings settings;
        settings.GTAOEnabled = true;
        settings.ActiveAOTechnique = AOTechnique::GTAO;
        settings.GTAODenoiseEnabled = false;
        settings.GTAODenoisePasses = 2;
        gtao->SetSettings(settings);
        const u64 off = data.Pipeline->ComputeDeclarationKey(data);

        settings.GTAODenoiseEnabled = true;
        gtao->SetSettings(settings);
        const u64 on = data.Pipeline->ComputeDeclarationKey(data);
        EXPECT_NE(on, off) << "Turning the GTAO denoise chain on changes what GTAORenderPass::Setup declares.";

        // How MANY denoise passes run is Execute's business: the same two
        // targets are declared for 2 passes as for 4.
        settings.GTAODenoisePasses = 4;
        gtao->SetSettings(settings);
        EXPECT_EQ(data.Pipeline->ComputeDeclarationKey(data), on)
            << "The denoise pass count is execution-only; hashing it would rebuild the graph per slider step.";

        // ...but zero passes is the gate itself.
        settings.GTAODenoisePasses = 0;
        gtao->SetSettings(settings);
        EXPECT_NE(data.Pipeline->ComputeDeclarationKey(data), on);
    }

    // #1333, U6: a particle pass without a render callback declares nothing.
    // Only a Scene frame sets the callback, so a renderer-only frame would
    // otherwise cache a culled particle node for the whole session.
    TEST(RenderGraphFingerprint, ParticleCallbackAndOITContributorsMoveTheKey)
    {
        {
            const auto [before, after] = Access::KeyAcross(
                [](Access::Data& d)
                {
                    auto particle = Ref<ParticleRenderPass>::Create();
                    d.Pipeline->SceneCompositePasses.Particle = particle;
                    const u64 installed = d.Pipeline->ComputeDeclarationKey(d);
                    particle->SetRenderCallback([] {});
                    EXPECT_NE(d.Pipeline->ComputeDeclarationKey(d), installed)
                        << "Gaining a render callback changes what ParticleRenderPass::Setup declares.";
                });
            EXPECT_NE(before, after);
        }
        {
            Access::Data data;
            auto prepare = Ref<OITPrepareRenderPass>::Create();
            data.Pipeline->SceneCompositePasses.OITPrepare = prepare;
            prepare->SetEnabled(true);
            const u64 without = data.Pipeline->ComputeDeclarationKey(data);
            prepare->SetHasContributors(true);
            EXPECT_NE(data.Pipeline->ComputeDeclarationKey(data), without)
                << "OIT prepare/resolve declare nothing without a contributor.";
        }
    }

    // ReSTIR's reservoir history is extracted from the target the LAST spatial
    // pass wrote. The declaration input is that resolved choice, not the two
    // settings behind it: 1 and 3 spatial passes extract from the same
    // ping-pong target. And it is an input only while the tier can run.
    TEST(RenderGraphFingerprint, ReSTIRExtractionSourceIsTheResolvedChoiceAndOnlyWhenArmed)
    {
        Access::Data data;
        auto restir = Ref<ReSTIRDIPass>::Create();
        data.Pipeline->SceneCompositePasses.ReSTIRDI = restir;

        ReSTIRDISettings settings;
        settings.SpatialReuse = true;
        settings.SpatialPasses = 1u;
        restir->SetSettings(settings);
        const u32 one = restir->ReservoirExtractionSource();
        settings.SpatialPasses = 3u;
        restir->SetSettings(settings);
        EXPECT_EQ(restir->ReservoirExtractionSource(), one) << "1 and 3 spatial passes extract from the same target.";
        settings.SpatialPasses = 2u;
        restir->SetSettings(settings);
        EXPECT_NE(restir->ReservoirExtractionSource(), one);
        settings.SpatialReuse = false;
        restir->SetSettings(settings);
        EXPECT_EQ(restir->ReservoirExtractionSource(), 0u) << "Without spatial reuse the temporal target is the source.";

        // A pass that cannot run (here: no shaders, as on a device without ray
        // tracing) declares nothing from the source, so editing it must not
        // rebuild the graph there.
        ASSERT_FALSE(restir->IsReadyForExecution());
        const u64 before = data.Pipeline->ComputeDeclarationKey(data);
        settings.SpatialReuse = true;
        settings.SpatialPasses = 2u;
        restir->SetSettings(settings);
        EXPECT_EQ(data.Pipeline->ComputeDeclarationKey(data), before);
    }

    // A render-stream pass whose Setup() declares nothing when its command
    // bucket is empty (issue #1315): a graph compiled during a frame with no
    // draws caches a node with no reads and no writes, and reachability culls
    // it. Gaining the first draw must move the key.
    TEST(RenderGraphFingerprint, EveryBucketGatedStreamPassChangesFingerprintOnFirstDraw)
    {
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

        for (const Case& c : cases)
        {
            const auto fp = Access::FingerprintAcrossFirstDraw(c.Stream);
            EXPECT_NE(fp.Empty, fp.WithDraw)
                << c.Name
                << " gates its Setup() declarations on an empty command bucket, but its "
                   "bucket state does not reach the declaration key. The graph keeps the "
                   "cached build in which the pass declared nothing, reachability culls it, and "
                   "everything it was asked to draw is silently absent from the frame (#1315).";

            // Stability: the cache exists to be hit.
            EXPECT_EQ(fp.Restated, fp.WithDraw) << c.Name;
        }
    }

    // -------------------------------------------------------------------------
    // Identities.
    // -------------------------------------------------------------------------

    // PopulateBlackboard imports the IBL textures. Switching scenes destroys
    // the old EnvironmentMap and builds a new one; with the import frozen the
    // graph went on binding DELETED textures, thousands of GL errors a second.
    TEST(RenderGraphFingerprint, ChangingAnImportedIBLTextureIdChangesFingerprint)
    {
        const Access::ImportedIBL base{ .Irradiance = TestHandle(26u), .Prefilter = TestHandle(45u), .BRDFLut = TestHandle(46u), .Environment = TestHandle(12u) };
        const u64 baseFp = Access::FingerprintWithIBL(base);

        {
            Access::ImportedIBL changed = base;
            changed.Irradiance = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp);
        }
        {
            Access::ImportedIBL changed = base;
            changed.Prefilter = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp);
        }
        {
            Access::ImportedIBL changed = base;
            changed.BRDFLut = TestHandle(99u);
            EXPECT_NE(Access::FingerprintWithIBL(changed), baseFp);
        }

        // Teardown: a scene with no environment map clears the identities.
        EXPECT_NE(Access::FingerprintWithIBL({}), baseFp);

        // THE RECYCLED-NAME CASE. A destroyed texture's slot reissued to its
        // replacement keeps the Index and advances the Generation; to anything
        // keyed on a driver name the two are the same texture.
        {
            Access::ImportedIBL recycled = base;
            recycled.Irradiance = TestHandle(26u, 2u);
            EXPECT_NE(Access::FingerprintWithIBL(recycled), baseFp)
                << "A reissued identity (same slot, new generation) must re-import, or the graph keeps "
                   "binding the destroyed resource under a name that now means something else.";
        }

        EXPECT_EQ(Access::FingerprintWithIBL(base), baseFp) << "Same identities, same key.";
    }

    // -------------------------------------------------------------------------
    // Execution-only inputs: they must NOT move the key (AC1, second half).
    // -------------------------------------------------------------------------

    // Each of these is read only by Execute() or a UBO upload. Moving the key
    // for one of them rebuilds the whole frame graph whenever it changes, and
    // the camera and jitter change EVERY frame.
    TEST(RenderGraphFingerprint, ExecutionOnlyInputsLeaveTheKeyAlone)
    {
        const std::vector<Toggle> executionOnly = {
            { "ViewMatrix", [](Access::Data& d)
              { d.ViewMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, -2.0f)); } },
            { "ProjectionMatrix", [](Access::Data& d)
              { d.ProjectionMatrix = glm::perspective(0.9f, 1.7f, 0.1f, 500.0f); } },
            { "ViewProjectionMatrix", [](Access::Data& d)
              { d.ViewProjectionMatrix = glm::perspective(0.9f, 1.7f, 0.1f, 500.0f); } },
            { "PrevViewProjectionMatrix", [](Access::Data& d)
              { d.PrevViewProjectionMatrix = glm::perspective(0.8f, 1.7f, 0.1f, 500.0f); } },
            { "ViewPos", [](Access::Data& d)
              { d.ViewPos = glm::vec3(12.0f, 4.0f, -30.0f); } },
            { "CurrJitterUV", [](Access::Data& d)
              { d.CurrJitterUV = glm::vec2(0.25f, -0.125f); } },
            { "TAAJitterFrameIndex", [](Access::Data& d)
              { d.TAAJitterFrameIndex += 7u; } },
            { "StochasticFrameIndex", [](Access::Data& d)
              { d.StochasticFrameIndex += 7u; } },
            { "CloudFrameIndex", [](Access::Data& d)
              { d.CloudFrameIndex += 7u; } },
            { "BloomThreshold", [](Access::Data& d)
              { d.PostProcess.BloomThreshold += 0.5f; } },
            { "Exposure", [](Access::Data& d)
              { d.PostProcess.Exposure *= 2.0f; } },
            { "SSR value params", [](Access::Data& d)
              {
                  d.PostProcess.SSRIntensity += 1.5f;
                  d.PostProcess.SSRMaxDistance += 25.0f;
              } },
            { "MotionBlurStrength", [](Access::Data& d)
              { d.PostProcess.MotionBlurStrength += 0.25f; } },
            // #1333, O1-O3: hashed by the old fingerprint, read only in Execute.
            { "GlobalEnvironmentMapID", [](Access::Data& d)
              { d.GlobalEnvironmentMapID = TestHandle(77u); } },
            { "RayTracedReflection.TierDebugView", [](Access::Data& d)
              { d.PostProcess.RayTracedReflection.TierDebugView = !d.PostProcess.RayTracedReflection.TierDebugView; } },
            { "SkinDiffusion.Quality", [](Access::Data& d)
              { d.SkinDiffusion.Quality = static_cast<decltype(d.SkinDiffusion.Quality)>(std::to_underlying(d.SkinDiffusion.Quality) + 1); } },
        };

        for (const Toggle& t : executionOnly)
        {
            const auto [before, after] = Access::KeyAcross(
                [&t](Access::Data& d)
                { t.Apply(d); },
                /*withGraph=*/true);
            EXPECT_EQ(before, after)
                << "'" << t.Name << "' is execution-only, yet it moved the declaration key: every change to it "
                                    "would rebuild the blackboard and every pass's Setup() for a graph that declares the same.";
        }
    }

    // #1333, O4: a raw setting that PopulateBlackboard never reads, only a
    // pass's resolved enable does, is not a declaration input on its own. With
    // no pass to arm (a device without the feature, or a path it does not run
    // on) toggling it cannot change a single declaration, and the old
    // fingerprint rebuilt the graph for it anyway.
    TEST(RenderGraphFingerprint, RawTogglesBehindAPassVerdictDoNotRebuildWithoutThePass)
    {
        const std::vector<Toggle> verdictOnly = {
            { "SSREnabled", [](Access::Data& d)
              { d.PostProcess.SSREnabled = !d.PostProcess.SSREnabled; } },
            { "SSGIEnabled", [](Access::Data& d)
              { d.PostProcess.SSGIEnabled = !d.PostProcess.SSGIEnabled; } },
            { "BloomEnabled", [](Access::Data& d)
              { d.PostProcess.BloomEnabled = !d.PostProcess.BloomEnabled; } },
            { "ContactShadowEnabled", [](Access::Data& d)
              { d.PostProcess.ContactShadowEnabled = !d.PostProcess.ContactShadowEnabled; } },
            { "RayTracedReflection.Enabled", [](Access::Data& d)
              { d.PostProcess.RayTracedReflection.Enabled = !d.PostProcess.RayTracedReflection.Enabled; } },
            { "GpuPathTracer.Enabled", [](Access::Data& d)
              { d.PostProcess.GpuPathTracer.Enabled = !d.PostProcess.GpuPathTracer.Enabled; } },
        };
        for (const Toggle& t : verdictOnly)
        {
            const auto [before, after] = Access::KeyAcross([&t](Access::Data& d)
                                                           { t.Apply(d); });
            EXPECT_EQ(before, after)
                << "'" << t.Name << "' reaches a declaration only through its pass's IsEnabled(), which "
                                    "ConfigurePassesForFrame resolves; with no pass it must not rebuild the graph.";
        }
    }

    // Colour-vision adaptation (issue #458) is a process-global player
    // preference rather than a PostProcessSettings field.
    TEST(RenderGraphFingerprint, ColorBlindModeTogglesFingerprint)
    {
        const RendererSettings rs = DeferredSettings();
        const PostProcessSettings pp;

        Accessibility::Reset();
        const u64 offFp = Access::Fingerprint(pp, rs);

        AccessibilitySettings on;
        on.ColorBlind = ColorBlindMode::Deuteranopia;
        Accessibility::Set(on);
        const u64 onFp = Access::Fingerprint(pp, rs);

        // Value-only knobs ride the UBO and must NOT perturb the key.
        AccessibilitySettings tweaked = on;
        tweaked.ColorBlindSeverity = 0.35f;
        tweaked.ColorBlindMethod = ColorBlindAdaptation::Simulate;
        Accessibility::Set(tweaked);
        const u64 tweakedFp = Access::Fingerprint(pp, rs);

        Accessibility::Reset();

        EXPECT_NE(onFp, offFp);
        EXPECT_EQ(tweakedFp, onFp);
    }
} // namespace OloEngine::Tests
