// OLO_TEST_LAYER: L5

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// =============================================================================
// Render-graph identity and cache ownership (issue #1333, acceptance criterion
// 3), plus the compiled-plan digest the declaration-cache verifier compares.
//
// Pass objects outlive a topology reset: the pipeline re-registers the SAME
// instances into the freshly reset graph, and every one of them may still hold
// a handle it latched in an earlier Setup(). Native resource names outlive
// their resources: GL reissues a deleted texture's name to the next texture
// created. Either can put a stale handle in front of a consumer, and neither
// produces an error, only a frame drawn from the wrong resource. These tests
// pin that neither can revive a handle, and that a reset invalidates the
// caches that depend on what it wiped, and only those.
// =============================================================================

namespace OloEngine::Tests
{
    namespace
    {
        class DeclaringNode : public RenderGraphNode
        {
          public:
            using SetupFn = std::function<void(DeclaringNode&, RGBuilder&)>;

            DeclaringNode(std::string name, SetupFn setup) : m_Setup(std::move(setup))
            {
                SetName(name);
            }

            void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
            {
                RenderGraphNode::Setup(builder, blackboard);
                ++SetupCount;
                if (m_Setup)
                    m_Setup(*this, builder);
            }

            void Execute(RGCommandContext& /*context*/) override {}

            // What a production pass does with m_SelectedX: keep a handle from
            // Setup() for Execute() to resolve.
            RGTextureHandle Latched{};
            u32 SetupCount = 0;
            bool DeclareSecondRead = false;
            u32 TargetWidth = 64u;

          private:
            SetupFn m_Setup;
        };

        [[nodiscard]] RGResourceDesc TextureDesc(std::string_view name, u32 width)
        {
            auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Texture2D, name);
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = width;
            desc.Height = width;
            return desc;
        }

        // Two passes: a producer writing a transient, and a consumer reading
        // it plus an imported texture. The consumer is the final pass. The
        // transients are declared on the graph before the build and read from
        // shared handles, the way PopulateBlackboard and the blackboard do it.
        struct SharedHandles
        {
            RGTextureHandle Scratch{};
            RGTextureHandle Output{};
        };

        struct TwoPassGraph
        {
            Ref<DeclaringNode> Producer;
            Ref<DeclaringNode> Consumer;
            std::shared_ptr<SharedHandles> Shared;

            // The populate step: declare the transients at their current size.
            void Declare(RenderGraph& graph) const
            {
                Shared->Scratch = graph.DeclareTransientTexture("Scratch", TextureDesc("Scratch", Producer->TargetWidth));
                Shared->Output = graph.DeclareTransientTexture("Output", TextureDesc("Output", 64u));
            }

            void Register(RenderGraph& graph) const
            {
                graph.AddNode(Producer);
                graph.AddNode(Consumer);
                graph.SetFinalPass(std::string(Consumer->GetName()));
                Declare(graph);
            }
        };

        [[nodiscard]] TwoPassGraph MakeTwoPassGraph(RHI::ResourceHandle imported)
        {
            TwoPassGraph out;
            out.Shared = std::make_shared<SharedHandles>();
            out.Producer = Ref<DeclaringNode>::Create(
                "Producer",
                [shared = out.Shared](DeclaringNode&, RGBuilder& builder)
                { builder.Write(shared->Scratch, RGWriteUsage::ShaderImage); });
            out.Consumer = Ref<DeclaringNode>::Create(
                "Consumer",
                [shared = out.Shared, imported](DeclaringNode& self, RGBuilder& builder)
                {
                    [[maybe_unused]] const auto scratchRead = builder.Read(shared->Scratch);
                    self.Latched = builder.ImportTextureHandle("Environment", imported, TextureDesc("Environment", 32u));
                    if (self.DeclareSecondRead)
                    {
                        [[maybe_unused]] const auto environmentRead = builder.Read(self.Latched);
                    }
                    builder.Write(shared->Output);
                });
            out.Consumer->SetSideEffects(RenderGraphNode::SideEffect::Present);
            return out;
        }

        [[nodiscard]] std::vector<std::string> DifferingLabels(const std::vector<RenderGraph::PlanDigestEntry>& a,
                                                               const std::vector<RenderGraph::PlanDigestEntry>& b)
        {
            std::vector<std::string> out;
            for (const auto& entry : a)
            {
                const auto it = std::ranges::find(b, entry.Label, &RenderGraph::PlanDigestEntry::Label);
                if (it == b.end() || it->Digest != entry.Digest)
                    out.push_back(entry.Label);
            }
            for (const auto& entry : b)
            {
                if (std::ranges::find(a, entry.Label, &RenderGraph::PlanDigestEntry::Label) == a.end())
                    out.push_back(entry.Label);
            }
            return out;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // ResetTopology invalidates the right owners.
    // -------------------------------------------------------------------------

    // The pipeline keys its caches on the graph's topology generation. With a
    // per-graph counter, every graph started at the same value, so a pipeline
    // that outlived a graph (or tests that create one per case) could see a
    // NEW graph report the generation its cached blackboard was built against.
    TEST(RenderGraphDeclarationIdentity, TopologyGenerationsNeverRepeatAcrossGraphInstances)
    {
        std::vector<u64> seen;
        {
            RenderGraph first;
            seen.push_back(first.GetTopologyGeneration());
            first.ResetTopology();
            seen.push_back(first.GetTopologyGeneration());
        }
        RenderGraph second;
        seen.push_back(second.GetTopologyGeneration());
        second.ResetTopology();
        seen.push_back(second.GetTopologyGeneration());
        second.Shutdown();
        seen.push_back(second.GetTopologyGeneration());

        std::vector<u64> sorted = seen;
        std::ranges::sort(sorted);
        EXPECT_EQ(std::ranges::adjacent_find(sorted), sorted.end())
            << "Two topologies reported the same generation; a cache keyed on it cannot tell them apart.";
        EXPECT_TRUE(std::ranges::all_of(seen, [](u64 generation)
                                        { return generation != 0u; }));
    }

    // The graph owns the BuildFrameGraph cache; ResetTopology wipes the
    // declarations that cache would replay, so it must invalidate it even when
    // the caller's key is unchanged.
    TEST(RenderGraphDeclarationIdentity, ResetTopologyInvalidatesTheBuildCacheEvenUnderTheSameKey)
    {
        RenderGraph graph;
        auto nodes = MakeTwoPassGraph(RHI::ResourceHandle{ 7u, 1u });
        nodes.Register(graph);

        constexpr u64 kKey = 0x1333u;
        graph.BuildFrameGraph(kKey);
        EXPECT_TRUE(graph.HasValidBuildFrameGraphCache(kKey));
        EXPECT_FALSE(graph.HasValidBuildFrameGraphCache(kKey + 1u)) << "A different key must never hit.";

        graph.BuildFrameGraph(kKey);
        EXPECT_EQ(graph.GetBuildCacheCounters().Compiles, 1u);
        EXPECT_EQ(graph.GetBuildCacheCounters().CacheHits, 1u);
        EXPECT_EQ(nodes.Consumer->SetupCount, 1u) << "A cache hit must not re-run Setup().";

        graph.ResetTopology();
        EXPECT_FALSE(graph.HasValidBuildFrameGraphCache(kKey));

        nodes.Register(graph);
        graph.BuildFrameGraph(kKey);
        EXPECT_EQ(graph.GetBuildCacheCounters().Compiles, 2u)
            << "The same key after a reset must recompile: the declarations it named were wiped.";
        EXPECT_EQ(nodes.Consumer->SetupCount, 2u);
    }

    // -------------------------------------------------------------------------
    // Retained pass objects and recycled native names cannot revive handles.
    // -------------------------------------------------------------------------

    // A pass object survives ResetTopology with the handle it latched in its
    // last Setup(). That handle must be dead afterwards, even though the SAME
    // name is re-imported for the SAME identity and the slot is reused by name.
    TEST(RenderGraphDeclarationIdentity, AHandleLatchedBeforeAResetIsDeadAfterIt)
    {
        auto& registry = RHI::ResourceRegistry::Get();
        const auto environment = registry.Register(RHI::ResourceKind::Texture, 7201u, RHI::Backend::OpenGL);

        RenderGraph graph;
        auto nodes = MakeTwoPassGraph(environment);
        nodes.Register(graph);
        graph.BuildFrameGraph(1u);
        const RGTextureHandle latchedBefore = nodes.Consumer->Latched;
        ASSERT_TRUE(graph.IsTextureHandleCurrent(latchedBefore));
        ASSERT_EQ(graph.ResolveTextureHandle(latchedBefore), environment);

        graph.ResetTopology();
        EXPECT_FALSE(graph.IsTextureHandleCurrent(latchedBefore))
            << "A retained pass kept a handle across a topology reset and it still resolves.";

        nodes.Register(graph);
        graph.BuildFrameGraph(1u);
        const RGTextureHandle latchedAfter = nodes.Consumer->Latched;
        EXPECT_TRUE(graph.IsTextureHandleCurrent(latchedAfter));
        EXPECT_EQ(graph.ResolveTextureHandle(latchedAfter), environment);
        EXPECT_FALSE(graph.IsTextureHandleCurrent(latchedBefore))
            << "Re-importing the same name for the same identity revived the handle from before the reset. "
               "The slot is reused by name, so only its generation stands between a retained pass and the "
               "resource the slot holds now.";
        EXPECT_FALSE(graph.ResolveTextureHandle(latchedBefore).IsValid());

        registry.Unregister(environment);
    }

    // THE NEGATIVE CONTROL THAT RECYCLES A NAME. The registry is asked for a
    // texture, the texture is destroyed, and a new one is registered under the
    // same native GL name, which is what the driver does to a deleted name.
    // Imported by identity, the old handle must die. Imported by native name,
    // it survives: that second half is the hazard, asserted here so it is
    // visible, and the reason every engine import carries an identity.
    TEST(RenderGraphDeclarationIdentity, ARecycledNativeNameCannotReviveAHandleImportedByIdentity)
    {
        constexpr u32 kRecycledGLName = 7301u;
        auto& registry = RHI::ResourceRegistry::Get();
        const auto first = registry.Register(RHI::ResourceKind::Texture, kRecycledGLName, RHI::Backend::OpenGL);
        const auto desc = TextureDesc("Environment", 32u);

        RenderGraph graph;
        const auto before = graph.ImportTextureHandle("Environment", first, desc);
        ASSERT_TRUE(graph.IsTextureHandleCurrent(before));

        registry.Unregister(first);
        const auto second = registry.Register(RHI::ResourceKind::Texture, kRecycledGLName, RHI::Backend::OpenGL);
        ASSERT_FALSE(first == second)
            << "The registry reissued the SAME identity for a recycled native name; identities would be no "
               "better than GL names.";

        const auto after = graph.ImportTextureHandle("Environment", second, desc);
        EXPECT_TRUE(graph.IsTextureHandleCurrent(after));
        EXPECT_FALSE(graph.IsTextureHandleCurrent(before))
            << "The texture behind the name was destroyed and recreated under the same GL name; the handle "
               "to the destroyed one must not resolve to its replacement.";
        EXPECT_EQ(graph.ResolveTextureHandle(after), second);

        // The control: the same sequence through the native-name import.
        const auto nativeBefore = graph.ImportTexture("EnvironmentNative", kRecycledGLName, desc);
        const auto nativeAfter = graph.ImportTexture("EnvironmentNative", kRecycledGLName, desc);
        EXPECT_TRUE(graph.IsTextureHandleCurrent(nativeBefore) && nativeBefore.Index == nativeAfter.Index &&
                    nativeBefore.Generation == nativeAfter.Generation)
            << "Expected the native-name import to be BLIND to recycling (this is the control). If it now "
               "detects it, update this test and the #1333 doc: the hazard it documents is gone.";

        registry.Unregister(second);
    }

    // -------------------------------------------------------------------------
    // The compiled-plan digest the declaration-cache verifier compares.
    // -------------------------------------------------------------------------

    // Two compiles from the same declarations must digest equal, although the
    // second reallocates every transient handle: otherwise the verifier would
    // report every verified frame as stale.
    TEST(RenderGraphDeclarationIdentity, PlanDigestIsStableAcrossRecompilesOfTheSameDeclarations)
    {
        RenderGraph graph;
        auto nodes = MakeTwoPassGraph(RHI::ResourceHandle{ 9u, 1u });
        nodes.Register(graph);

        graph.BuildFrameGraph(1u);
        std::vector<RenderGraph::PlanDigestEntry> firstEntries;
        const u64 first = graph.ComputeCompiledPlanDigest(&firstEntries);

        graph.InvalidateBuildFrameGraphCache();
        graph.BuildFrameGraph(1u);
        std::vector<RenderGraph::PlanDigestEntry> secondEntries;
        const u64 second = graph.ComputeCompiledPlanDigest(&secondEntries);

        EXPECT_EQ(nodes.Consumer->SetupCount, 2u) << "The second build must really have recompiled.";
        EXPECT_EQ(first, second) << "Differing: " << ::testing::PrintToString(DifferingLabels(firstEntries, secondEntries));
        EXPECT_FALSE(firstEntries.empty());
    }

    // A changed declaration and a changed descriptor must each move the digest,
    // and the entries must name which pass or resource moved: the verifier's
    // error message is only useful if it names the culprit.
    TEST(RenderGraphDeclarationIdentity, PlanDigestNamesThePassAndResourceThatChanged)
    {
        RenderGraph graph;
        auto nodes = MakeTwoPassGraph(RHI::ResourceHandle{ 11u, 1u });
        nodes.Register(graph);

        graph.BuildFrameGraph(1u);
        std::vector<RenderGraph::PlanDigestEntry> baseEntries;
        const u64 base = graph.ComputeCompiledPlanDigest(&baseEntries);

        nodes.Consumer->DeclareSecondRead = true;
        graph.InvalidateBuildFrameGraphCache();
        graph.BuildFrameGraph(1u);
        std::vector<RenderGraph::PlanDigestEntry> readEntries;
        EXPECT_NE(graph.ComputeCompiledPlanDigest(&readEntries), base);
        const auto readDiff = DifferingLabels(baseEntries, readEntries);
        EXPECT_NE(std::ranges::find(readDiff, std::string("pass:Consumer")), readDiff.end())
            << ::testing::PrintToString(readDiff);
        EXPECT_EQ(std::ranges::find(readDiff, std::string("pass:Producer")), readDiff.end())
            << "The producer declared nothing different: " << ::testing::PrintToString(readDiff);

        nodes.Consumer->DeclareSecondRead = false;
        nodes.Producer->TargetWidth = 128u;
        nodes.Declare(graph);
        graph.InvalidateBuildFrameGraphCache();
        graph.BuildFrameGraph(1u);
        std::vector<RenderGraph::PlanDigestEntry> sizeEntries;
        EXPECT_NE(graph.ComputeCompiledPlanDigest(&sizeEntries), base);
        const auto sizeDiff = DifferingLabels(baseEntries, sizeEntries);
        EXPECT_NE(std::ranges::find(sizeDiff, std::string("resource:Scratch")), sizeDiff.end())
            << "A descriptor change must be named by resource: " << ::testing::PrintToString(sizeDiff);
    }
} // namespace OloEngine::Tests
