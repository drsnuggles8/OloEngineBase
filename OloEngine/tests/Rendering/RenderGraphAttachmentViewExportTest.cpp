// OLO_TEST_LAYER: L5
// =============================================================================
// RenderGraphAttachmentViewExportTest.cpp — issue #1332.
//
// The forward scene exports (SceneDepth, SceneNormals, Velocity) used to be
// transient textures that ScenePass filled by copying its attachments, and
// that every later geometry writer had to copy again. A writer that drew
// after the copy and did not refresh it left every consumer reading the
// attachment as it was BEFORE that writer. Since #1332 they are attachment
// views of SceneColor, so a write to the framebuffer IS a write to them.
//
// These pin the graph contract that makes the view model correct, on a graph
// with no GPU:
//   * a consumer of a view is ordered after every writer of the framebuffer
//     registered before it, and the version it reads names that writer
//     (the version assertion the copy model cannot pass);
//   * a reader of a view runs before a later versioned write of its
//     framebuffer, which overwrites the texels it sampled;
//   * a temporal history extracted from a view keeps the framebuffer alive
//     to the end of the frame, so the end-of-frame copy cannot read storage
//     the transient planner already handed to another resource.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    class LambdaNode : public RenderGraphNode
    {
      public:
        using SetupFn = std::function<void(RGBuilder&)>;

        LambdaNode(std::string name, SetupFn setup) : m_Name(std::move(name)), m_Setup(std::move(setup)) {}

        [[nodiscard]] std::string_view GetName() const override
        {
            return m_Name;
        }
        void Setup(RGBuilder& builder, FrameBlackboard& /*blackboard*/) override
        {
            if (m_Setup)
                m_Setup(builder);
        }
        void Execute(RGCommandContext& /*context*/) override {}

      private:
        std::string m_Name;
        SetupFn m_Setup;
    };

    void Add(RenderGraph& graph, std::string name, LambdaNode::SetupFn setup)
    {
        graph.AddNode(Ref<LambdaNode>::Create(std::move(name), std::move(setup)));
    }

    // The forward scene target's layout (SceneRenderPass::SceneMRTAttachments).
    [[nodiscard]] RGResourceDesc SceneTargetDesc()
    {
        RGResourceDesc desc;
        desc.Kind = RGResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::R32Int, RGResourceFormat::RG16Float,
                             RGResourceFormat::RGBA16Float, RGResourceFormat::RGBA16Float,
                             RGResourceFormat::Depth24Stencil8 };
        return desc;
    }

    [[nodiscard]] RGResourceDesc TextureDesc(const RGResourceFormat format)
    {
        RGResourceDesc desc;
        desc.Kind = RGResourceHandle::Kind::Texture2D;
        desc.Format = format;
        desc.Width = 640u;
        desc.Height = 360u;
        return desc;
    }

    [[nodiscard]] std::ptrdiff_t IndexOf(const RenderGraph& graph, std::string_view pass)
    {
        const auto order = graph.GetExecutionOrder();
        const auto it = std::ranges::find_if(order, [pass](const FString& name)
                                             { return name.ToView() == pass; });
        return it == order.end() ? -1 : std::distance(order.begin(), it);
    }

    constexpr std::array<std::string_view, 3> kExports = { "SceneDepth", "SceneNormals", "Velocity" };
} // namespace

// -----------------------------------------------------------------------------
// AC2, the view model: ScenePass draws, a late geometry writer draws after it
// (foliage, groom, the GPU-driven batches, water), and a consumer registered
// after both reads the three exports. The version each export carries at the
// consumer's declaration is the late writer's, and the consumer runs after it.
// -----------------------------------------------------------------------------
TEST(RenderGraphAttachmentViewExports, AConsumerReadsTheVersionTheLastFramebufferWriterLeft)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    const auto depth = graph.CreateFramebufferDepthAttachmentView("SceneDepth", scene);
    const auto normals = graph.CreateFramebufferAttachmentView("SceneNormals", scene, 2u);
    const auto velocity = graph.CreateFramebufferAttachmentView("Velocity", scene, 3u);
    ASSERT_TRUE(depth.IsValid() && normals.IsValid() && velocity.IsValid());

    Add(graph, "ScenePass", [scene](RGBuilder& builder)
        { builder.Write(scene, RGWriteUsage::RenderTarget); });
    Add(graph, "LateGeometryPass", [scene](RGBuilder& builder)
        {
            [[maybe_unused]] const auto read = builder.Read(scene, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "LateGeometryPass");
            builder.DependsOnPreviousWriter("SceneColor"); });

    std::map<std::string, std::string, std::less<>> versionAtConsumer;
    Add(graph, "ConsumerPass", [&graph, &versionAtConsumer, depth, normals, velocity](RGBuilder& builder)
        {
            for (const std::string_view name : kExports)
                versionAtConsumer[std::string(name)] = graph.GetLastWriterPassName(name).ToStdString();
            for (const auto view : { depth, normals, velocity })
            {
                [[maybe_unused]] const auto read = builder.Read(view, RGReadUsage::ShaderSample);
            }
            builder.Write(builder.CreateTexture("ConsumerOut", TextureDesc(RGResourceFormat::RGBA16Float)),
                          RGWriteUsage::RenderTarget); });
    Add(graph, "FinalPass", [](RGBuilder& builder)
        {
            const auto out = builder.CreateTexture("ConsumerOut", TextureDesc(RGResourceFormat::RGBA16Float));
            [[maybe_unused]] const auto read = builder.Read(out, RGReadUsage::ShaderSample); });

    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();
    graph.Execute();

    for (const std::string_view name : kExports)
    {
        EXPECT_EQ(versionAtConsumer[std::string(name)], "LateGeometryPass")
            << name << " reached the consumer as another pass's version: the late writer's geometry is missing from it";
    }
    const auto late = IndexOf(graph, "LateGeometryPass");
    const auto consumer = IndexOf(graph, "ConsumerPass");
    ASSERT_GE(late, 0);
    ASSERT_GE(consumer, 0);
    EXPECT_LT(late, consumer) << "the consumer ran before the late writer it must see";
    EXPECT_TRUE(graph.ValidateCompiledResourceHazards().IsEmpty()) << "views of one framebuffer read and written as declared "
                                                                      "must validate clean";
}

// -----------------------------------------------------------------------------
// The copy model, for contrast: the same frame with SceneDepth a separate
// texture that ScenePass fills by copying. The late writer draws into the
// framebuffer but cannot see the copy, so the consumer's version is still
// ScenePass's -- the stale export #1332 removes. Pinned so the test above
// cannot pass for a reason that would also pass this one.
// -----------------------------------------------------------------------------
TEST(RenderGraphAttachmentViewExports, ACopiedExportStaysAtTheCopyingPassesVersion)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    std::string versionAtConsumer;

    Add(graph, "ScenePass", [scene](RGBuilder& builder)
        {
            builder.Write(scene, RGWriteUsage::RenderTarget);
            builder.Write(builder.CreateTexture("SceneDepth", TextureDesc(RGResourceFormat::Depth24Stencil8)),
                          RGWriteUsage::TransferDest); });
    Add(graph, "LateGeometryPass", [scene](RGBuilder& builder)
        {
            [[maybe_unused]] const auto read = builder.Read(scene, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "LateGeometryPass");
            builder.DependsOnPreviousWriter("SceneColor"); });
    Add(graph, "ConsumerPass", [&graph, &versionAtConsumer](RGBuilder& builder)
        {
            versionAtConsumer = graph.GetLastWriterPassName("SceneDepth").ToStdString();
            const auto copy = builder.CreateTexture("SceneDepth", TextureDesc(RGResourceFormat::Depth24Stencil8));
            [[maybe_unused]] const auto read = builder.Read(copy, RGReadUsage::ShaderSample); });

    graph.SetFinalPass("ConsumerPass");
    graph.BuildFrameGraph();
    graph.Execute();

    EXPECT_EQ(versionAtConsumer, "ScenePass") << "the copied export does not carry the late writer's version; if this "
                                                 "changed, the copy model is no longer what the test above rules out";
}

// -----------------------------------------------------------------------------
// Write after read through a view: a decal-like pass samples SceneDepth (a view
// of SceneColor), and a late geometry pass registered after it redraws
// SceneColor under a new version name ("SceneColor@LateGeometryPass"). That
// write overwrites the depth the reader sampled, so the reader must run first.
// Keyed only by the exact resource name, the write found no reader of
// "SceneColor@LateGeometryPass" and the order fell to the tie-break; the
// reverse tie-break ran the late pass first. Run under both tie-breaks so the
// order cannot come from registration alone.
// -----------------------------------------------------------------------------
TEST(RenderGraphAttachmentViewExports, AViewReaderRunsBeforeALaterVersionedWriteOfItsFramebuffer)
{
    // Restored on every exit, an ASSERT's early return included.
    struct RestoreTieBreak
    {
        bool Was = Levers::RenderGraphReverseTieBreak();
        ~RestoreTieBreak()
        {
            Levers::SetRenderGraphReverseTieBreak(Was);
        }
    } const restore;
    for (const bool reverse : { false, true })
    {
        SCOPED_TRACE(reverse ? "reverse tie-break" : "forward tie-break");
        Levers::SetRenderGraphReverseTieBreak(reverse);

        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(false);

        const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
        const auto depth = graph.CreateFramebufferDepthAttachmentView("SceneDepth", scene);
        ASSERT_TRUE(depth.IsValid());

        Add(graph, "ScenePass", [scene](RGBuilder& builder)
            { builder.Write(scene, RGWriteUsage::RenderTarget); });
        Add(graph, "DepthReaderPass", [depth](RGBuilder& builder)
            {
                [[maybe_unused]] const auto read = builder.Read(depth, RGReadUsage::ShaderSample);
                builder.Write(builder.CreateTexture("ReaderOut", TextureDesc(RGResourceFormat::RGBA16Float)),
                              RGWriteUsage::RenderTarget); });
        Add(graph, "LateGeometryPass", [scene](RGBuilder& builder)
            {
                [[maybe_unused]] const auto read = builder.Read(scene, RGReadUsage::RenderTargetRead);
                [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "LateGeometryPass");
                builder.DependsOnPreviousWriter("SceneColor"); });
        Add(graph, "FinalPass", [scene](RGBuilder& builder)
            {
                [[maybe_unused]] const auto read = builder.Read(scene, RGReadUsage::ShaderSample);
                const auto out = builder.CreateTexture("ReaderOut", TextureDesc(RGResourceFormat::RGBA16Float));
                [[maybe_unused]] const auto readOut = builder.Read(out, RGReadUsage::ShaderSample); });

        graph.SetFinalPass("FinalPass");
        graph.BuildFrameGraph();
        graph.Execute();

        const auto reader = IndexOf(graph, "DepthReaderPass");
        const auto late = IndexOf(graph, "LateGeometryPass");
        ASSERT_GE(reader, 0);
        ASSERT_GE(late, 0);
        EXPECT_LT(reader, late) << "the late pass redrew SceneColor before the SceneDepth reader sampled it";
    }
}

// -----------------------------------------------------------------------------
// AC3: TAA extracts its surface history from Velocity at the end of the frame.
// Velocity is now a view, so the storage the extraction reads is SceneColor's.
// A history extracted from a view must keep the PARENT alive to the last pass,
// or the planner can give the framebuffer's slot to a later same-descriptor
// framebuffer and the end-of-frame copy reads that one's texels. The paired
// negative of RenderGraphTransientPool.NonOverlappingTransientResourcesReuseAliasSlot,
// for a view instead of a texture.
// -----------------------------------------------------------------------------
TEST(RenderGraphAttachmentViewExports, AHistoryExtractedFromAViewKeepsItsFramebufferAlive)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto sceneA = graph.DeclareTransientFramebuffer("SceneA", SceneTargetDesc());
    const auto velocityA = graph.CreateFramebufferAttachmentView("SceneAVelocity", sceneA, 3u);
    const auto sceneB = graph.DeclareTransientFramebuffer("SceneB", SceneTargetDesc());

    Add(graph, "A", [sceneA, velocityA](RGBuilder& builder)
        {
            builder.Write(sceneA, RGWriteUsage::RenderTarget);
            builder.ExtractHistoryTexture("SceneAVelocityHistory", velocityA); });
    Add(graph, "B", [velocityA](RGBuilder& builder)
        {
            [[maybe_unused]] const auto read = builder.Read(velocityA, RGReadUsage::ShaderSample);
            builder.Write(builder.CreateTexture("BOut", TextureDesc(RGResourceFormat::RGBA16Float)), RGWriteUsage::RenderTarget); });
    Add(graph, "C", [sceneB](RGBuilder& builder)
        { builder.Write(sceneB, RGWriteUsage::RenderTarget); });
    Add(graph, "Final", [sceneB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto read = builder.Read(sceneB, RGReadUsage::ShaderSample);
            const auto out = builder.CreateTexture("BOut", TextureDesc(RGResourceFormat::RGBA16Float));
            [[maybe_unused]] const auto readOut = builder.Read(out, RGReadUsage::ShaderSample); });

    graph.AddExecutionDependency("B", "C");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& plan = graph.GetTransientPlan();
    const auto find = [&plan](const std::string& resource) -> const RenderGraph::TransientPlanEntry*
    {
        const auto it = std::ranges::find_if(plan, [resource](const RenderGraph::TransientPlanEntry& entry)
                                             { return entry.Resource == resource; });
        return it == plan.end() ? nullptr : &(*it);
    };
    const auto* a = find("SceneA");
    const auto* b = find("SceneB");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(a->AliasGroup, b->AliasGroup) << "the setup needs identical descriptors, so only the lifetime keeps them apart";
    EXPECT_GE(a->LastPassIndex, b->LastPassIndex)
        << "a framebuffer whose attachment view is extracted after the last pass must stay live to it";
    EXPECT_NE(a->AliasSlot, b->AliasSlot)
        << "SceneA's velocity view is copied into a history AFTER every pass ran, so SceneA's backing must not be "
           "handed to SceneB; the copy would read SceneB's texels and the history would be silently wrong";
}

namespace
{
    [[nodiscard]] bool HasFeedback(RenderGraph& graph, std::string_view resource = {})
    {
        const auto hazards = graph.ValidateCompiledResourceHazards();
        return std::ranges::any_of(hazards, [resource](const RenderGraph::Hazard& hazard)
                                   { return hazard.Kind == RenderGraph::HazardKind::FeedbackWithoutDeclaration &&
                                            (resource.empty() || hazard.Resource.ToView() == resource); });
    }
} // namespace

TEST(RenderGraphAttachmentFeedback, AFramebufferRenameCannotHideTheHistoricalWaterNormalFeedback)
{
    RenderGraph graph;
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    const auto normals = graph.CreateFramebufferAttachmentView("SceneViewNormals", scene, 2u);
    Add(graph, "Water", [scene, normals](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(normals);
        [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "Water"); });
    graph.SetFinalPass("Water");
    graph.BuildFrameGraph();
    EXPECT_TRUE(HasFeedback(graph, "SceneViewNormals"));
}

TEST(RenderGraphAttachmentFeedback, IndependentlyNamedViewsOfOneAttachmentConflict)
{
    RenderGraph graph;
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    const auto readView = graph.CreateFramebufferAttachmentView("ReadView", scene, 2u);
    const auto writeView = graph.CreateFramebufferAttachmentView("WriteView", scene, 2u);
    Add(graph, "Feedback", [readView, writeView](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(readView);
        builder.Write(writeView); });
    graph.SetFinalPass("Feedback");
    graph.BuildFrameGraph();
    EXPECT_TRUE(HasFeedback(graph, "ReadView"));
}

TEST(RenderGraphAttachmentFeedback, AttachmentIndicesAndDepthAreDistinctPhysicalImages)
{
    for (const bool depthRead : { false, true })
    {
        SCOPED_TRACE(depthRead);
        RenderGraph graph;
        const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
        const auto readView = depthRead ? graph.CreateFramebufferDepthAttachmentView("ReadView", scene)
                                        : graph.CreateFramebufferAttachmentView("ReadView", scene, 4u);
        const auto writeView = graph.CreateFramebufferAttachmentView("WriteView", scene, 0u);
        Add(graph, "Disjoint", [readView, writeView](RGBuilder& builder)
            {
            [[maybe_unused]] const auto read = builder.Read(readView);
            builder.Write(writeView); });
        graph.SetFinalPass("Disjoint");
        graph.BuildFrameGraph();
        EXPECT_FALSE(HasFeedback(graph));
    }
}

TEST(RenderGraphAttachmentFeedback, IdenticalAttachmentIndicesOnDifferentFramebuffersDoNotConflict)
{
    RenderGraph graph;
    const auto lhs = graph.DeclareTransientFramebuffer("Lhs", SceneTargetDesc());
    const auto rhs = graph.DeclareTransientFramebuffer("Rhs", SceneTargetDesc());
    const auto readView = graph.CreateFramebufferAttachmentView("ReadView", lhs, 2u);
    Add(graph, "Disjoint", [readView, rhs](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(readView);
        builder.Write(rhs); });
    graph.SetFinalPass("Disjoint");
    graph.BuildFrameGraph();
    EXPECT_FALSE(HasFeedback(graph));
}

TEST(RenderGraphAttachmentFeedback, TransitiveFramebufferAndTextureVersionsKeepTheirPhysicalIdentity)
{
    RenderGraph graph;
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    const auto depth = graph.CreateFramebufferDepthAttachmentView("Depth", scene);
    Add(graph, "Feedback", [scene, depth](RGBuilder& builder)
        {
        const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "First");
        [[maybe_unused]] const auto last = builder.WriteNewVersion(next, RGWriteUsage::RenderTarget, "Second");
        const auto depthVersion = builder.WriteNewVersion(depth, RGWriteUsage::DepthStencil, "DepthFirst");
        [[maybe_unused]] const auto read = builder.Read(depthVersion); });
    graph.SetFinalPass("Feedback");
    graph.BuildFrameGraph();
    EXPECT_TRUE(HasFeedback(graph, "Depth@DepthFirst"));
}

TEST(RenderGraphAttachmentFeedback, DeclaredIterationIsMatchedByPhysicalAttachmentAndCannotCoverAnother)
{
    for (const bool correctAttachment : { false, true })
    {
        SCOPED_TRACE(correctAttachment);
        RenderGraph graph;
        const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
        const auto readView = graph.CreateFramebufferAttachmentView("ReadView", scene, 2u);
        const auto declaration = graph.CreateFramebufferAttachmentView("Declaration", scene, correctAttachment ? 2u : 0u);
        Add(graph, "Iteration", [scene, readView, declaration](RGBuilder& builder)
            {
            [[maybe_unused]] const auto read = builder.Read(readView);
            [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "Iteration");
            builder.AllowSamePassReadWrite(declaration); });
        graph.SetFinalPass("Iteration");
        graph.BuildFrameGraph();
        EXPECT_EQ(HasFeedback(graph), !correctAttachment);
    }
}

TEST(RenderGraphAttachmentFeedback, AttachmentLoadAndBlendReadsAreLegalAcrossRenames)
{
    RenderGraph graph;
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    [[maybe_unused]] const auto normals = graph.CreateFramebufferAttachmentView("Normals", scene, 2u);
    Add(graph, "Blend", [scene](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(scene, RGReadUsage::RenderTargetRead);
        [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "Blend"); });
    graph.SetFinalPass("Blend");
    graph.BuildFrameGraph();
    EXPECT_FALSE(HasFeedback(graph));
}

TEST(RenderGraphAttachmentFeedback, VersionedMipAndLayerViewsConflictOnlyWhenTheirParentRangesOverlap)
{
    for (const bool layerView : { false, true })
    {
        for (const bool overlap : { false, true })
        {
            SCOPED_TRACE(layerView);
            SCOPED_TRACE(overlap);
            RenderGraph graph;
            auto desc = TextureDesc(RGResourceFormat::RGBA16Float);
            desc.MipLevels = 4;
            desc.DepthOrLayers = 4;
            if (layerView)
                desc.Kind = RGResourceHandle::Kind::Texture2DArray;
            const auto parent = graph.DeclareTransientTexture("Parent", desc);
            const auto view = layerView ? graph.CreateTextureArrayLayerView("View", parent, 1u)
                                        : graph.CreateTextureMipView("View", parent, 1u);
            ASSERT_TRUE(view.IsValid());
            Add(graph, "Range", [parent, view, layerView, overlap](RGBuilder& builder)
                {
                [[maybe_unused]] const auto read = builder.Read(view);
                const auto range = layerView ? RGSubresourceRange::Layer(overlap ? 1u : 2u)
                                             : RGSubresourceRange::Mip(overlap ? 1u : 2u);
                [[maybe_unused]] const auto next = builder.WriteNewVersion(parent, RGWriteUsage::ShaderImage, "Write", range); });
            graph.SetFinalPass("Range");
            graph.BuildFrameGraph();
            EXPECT_EQ(HasFeedback(graph), overlap);
        }
    }
}

TEST(RenderGraphAttachmentFeedback, APartialFeedbackDeclarationDoesNotAuthorizeTheWholeOverlap)
{
    RenderGraph graph;
    auto desc = TextureDesc(RGResourceFormat::RGBA16Float);
    desc.MipLevels = 4;
    const auto texture = graph.DeclareTransientTexture("Texture", desc);
    Add(graph, "Partial", [texture](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(texture);
        [[maybe_unused]] const auto next = builder.WriteNewVersion(texture, RGWriteUsage::ShaderImage, "Write");
        builder.AllowSamePassReadWrite(texture, RGSubresourceRange::Mip(1u)); });
    graph.SetFinalPass("Partial");
    graph.BuildFrameGraph();
    EXPECT_TRUE(HasFeedback(graph));
}

TEST(RenderGraphAttachmentFeedback, ATransferBeforeWriteDeclarationCannotAuthorizeShaderSampling)
{
    for (const bool sampleAttachedImage : { false, true })
    {
        SCOPED_TRACE(sampleAttachedImage);
        RenderGraph graph;
        const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
        const auto color = graph.CreateFramebufferAttachmentView("Color", scene, 0u);
        Add(graph, "CopyThenDraw", [scene, color, sampleAttachedImage](RGBuilder& builder)
            {
            [[maybe_unused]] const auto copy = builder.ReadTransferSourceBeforeWrite(color);
            if (sampleAttachedImage)
            {
                [[maybe_unused]] const auto read = builder.Read(color);
            }
            [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "Draw"); });
        graph.SetFinalPass("CopyThenDraw");
        graph.BuildFrameGraph();
        EXPECT_EQ(HasFeedback(graph), sampleAttachedImage);
    }
}

TEST(RenderGraphAttachmentFeedback, ASeparateSnapshotOrdersBeforeTheAliasedWriterAndRemovesFeedback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    const auto normals = graph.CreateFramebufferAttachmentView("Normals", scene, 2u);
    const auto snapshot = graph.DeclareTransientTexture("Snapshot", TextureDesc(RGResourceFormat::RG16Float));
    Add(graph, "Opaque", [scene](RGBuilder& builder)
        { builder.Write(scene); });
    Add(graph, "Snapshot", [normals, snapshot](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(normals, RGReadUsage::TransferSource);
        builder.Write(snapshot, RGWriteUsage::TransferDest); });
    Add(graph, "Water", [scene, snapshot](RGBuilder& builder)
        {
        [[maybe_unused]] const auto read = builder.Read(snapshot);
        [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "Water"); });
    graph.SetFinalPass("Water");
    graph.BuildFrameGraph();
    graph.Execute();
    EXPECT_LT(IndexOf(graph, "Opaque"), IndexOf(graph, "Snapshot"));
    EXPECT_LT(IndexOf(graph, "Snapshot"), IndexOf(graph, "Water"));
    EXPECT_FALSE(HasFeedback(graph));
}

TEST(RenderGraphAttachmentFeedback, CubeFacesAndUnboundedRangesRespectTheirBaseAndEmptySpans)
{
    for (const u32 writeFace : { 0u, 1u })
    {
        RenderGraph graph;
        auto desc = TextureDesc(RGResourceFormat::RGBA16Float);
        desc.Kind = RGResourceHandle::Kind::TextureCube;
        desc.DepthOrLayers = 6u;
        const auto cube = graph.DeclareTransientTexture("Cube", desc);
        const auto face = graph.CreateTextureCubeFaceView("Face", cube, 1u);
        ASSERT_TRUE(face.IsValid());
        Add(graph, "FaceAccess", [cube, face, writeFace](RGBuilder& builder)
            {
            [[maybe_unused]] const auto read = builder.Read(face);
            RGSubresourceRange range;
            range.BaseSlice = writeFace;
            range.SliceCount = 1u;
            [[maybe_unused]] const auto next = builder.WriteNewVersion(cube, RGWriteUsage::ShaderImage, "Write", range); });
        graph.SetFinalPass("FaceAccess");
        graph.BuildFrameGraph();
        EXPECT_EQ(HasFeedback(graph), writeFace == 1u);
    }
    for (const u32 readCount : { 0u, ~0u })
    {
        RenderGraph graph;
        auto desc = TextureDesc(RGResourceFormat::RGBA16Float);
        desc.MipLevels = 4u;
        const auto texture = graph.DeclareTransientTexture("Texture", desc);
        Add(graph, "EmptyOrDisjoint", [texture, readCount](RGBuilder& builder)
            {
            RGSubresourceRange readRange;
            readRange.BaseMip = 1u;
            readRange.MipCount = readCount;
            [[maybe_unused]] const auto read = builder.Read(texture, RGReadUsage::ShaderSample, readRange);
            [[maybe_unused]] const auto next = builder.WriteNewVersion(texture, RGWriteUsage::ShaderImage, "Write", RGSubresourceRange::Mip(0u)); });
        graph.SetFinalPass("EmptyOrDisjoint");
        graph.BuildFrameGraph();
        EXPECT_FALSE(HasFeedback(graph));
    }
}

TEST(RenderGraphAttachmentFeedback, AResolveViewNamesTheSeparateSingleSampleBacking)
{
    for (const bool writeResolveBacking : { false, true })
    {
        SCOPED_TRACE(writeResolveBacking);
        RenderGraph graph;
        auto desc = TextureDesc(RGResourceFormat::RGBA16Float);
        const auto backing = graph.DeclareTransientTexture("Resolved", desc);
        desc.Samples = 4u;
        const auto multisample = graph.DeclareTransientTexture("Multisample", desc);
        const auto view = graph.CreateTextureMultisampleResolveView("ResolveView", multisample, backing);
        ASSERT_TRUE(view.IsValid());
        Add(graph, "ResolveAccess", [view, multisample, backing, writeResolveBacking](RGBuilder& builder)
            {
            [[maybe_unused]] const auto read = builder.Read(view);
            [[maybe_unused]] const auto next = builder.WriteNewVersion(writeResolveBacking ? backing : multisample,
                                                                       RGWriteUsage::RenderTarget, "Write"); });
        graph.SetFinalPass("ResolveAccess");
        graph.BuildFrameGraph();
        EXPECT_EQ(HasFeedback(graph), writeResolveBacking);
    }
}

TEST(RenderGraphAttachmentFeedback, ShaderDepthSamplingWhileWritingTheFramebufferRequiresASnapshotOnBothBackends)
{
    struct RestoreAPI
    {
        RendererAPI::API Previous = RendererAPI::GetAPI();
        ~RestoreAPI()
        {
            RendererAPI::SetAPI(Previous);
        }
    } const restore;
    // This is a declaration policy test, with no GPU/backend recreation. The
    // current Vulkan target binds writable depth/stencil; neither backend gets
    // an implicit exemption merely because a pass disables depth writes.
    for (const auto backend : { RendererAPI::API::OpenGL, RendererAPI::API::Vulkan })
    {
        RendererAPI::SetAPI(backend);
        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(false);
        const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
        const auto depth = graph.CreateFramebufferDepthAttachmentView("Depth", scene);
        Add(graph, "DepthSample", [scene, depth](RGBuilder& builder)
            {
            [[maybe_unused]] const auto read = builder.Read(depth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto next = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "Draw"); });
        graph.SetFinalPass("DepthSample");
        graph.BuildFrameGraph();
        EXPECT_TRUE(HasFeedback(graph, "Depth"));
    }
}

TEST(RenderGraphAttachmentViewExports, ATransitiveFramebufferRenameRepublishesTheCanonicalViews)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    const auto depth = graph.CreateFramebufferDepthAttachmentView("Depth", scene);
    ASSERT_TRUE(graph.CreateFramebufferAttachmentView("Color", scene, 0u).IsValid());
    RGFramebufferHandle first;
    Add(graph, "First", [scene, &first](RGBuilder& builder)
        { first = builder.WriteNewVersion(scene, RGWriteUsage::RenderTarget, "First"); });
    Add(graph, "Second", [&first](RGBuilder& builder)
        {
        [[maybe_unused]] const auto load = builder.Read(first, RGReadUsage::RenderTargetRead);
        [[maybe_unused]] const auto second = builder.WriteNewVersion(first, RGWriteUsage::RenderTarget, "Second"); });
    FString lastWriter;
    Add(graph, "Consumer", [&graph, &lastWriter, depth](RGBuilder& builder)
        {
        lastWriter = graph.GetLastWriterPassName("Depth");
        EXPECT_EQ(graph.GetResourceName(graph.GetTextureHandle("Depth")), "Depth@First@Second");
        EXPECT_EQ(graph.GetResourceName(graph.GetTextureHandle("Color")), "Color@First@Second");
        [[maybe_unused]] const auto read = builder.Read(depth); });
    graph.SetFinalPass("Consumer");
    graph.BuildFrameGraph();
    EXPECT_EQ(lastWriter, "Second");
    EXPECT_GE(IndexOf(graph, "Second"), 0);
    EXPECT_LT(IndexOf(graph, "Second"), IndexOf(graph, "Consumer"));
}

TEST(RenderGraphAttachmentViewExports, AViewCreatedInConsumerSetupRetainsTheEarlierFramebufferWriter)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    const auto scene = graph.DeclareTransientFramebuffer("SceneColor", SceneTargetDesc());
    Add(graph, "Opaque", [scene](RGBuilder& builder)
        { builder.Write(scene); });
    Add(graph, "Consumer", [scene](RGBuilder& builder)
        {
        const auto lateView = builder.CreateFramebufferAttachmentView("LateNormals", scene, 2u);
        [[maybe_unused]] const auto read = builder.Read(lateView); });
    graph.SetFinalPass("Consumer");
    graph.BuildFrameGraph();
    EXPECT_GE(IndexOf(graph, "Opaque"), 0);
    EXPECT_LT(IndexOf(graph, "Opaque"), IndexOf(graph, "Consumer"));
    EXPECT_FALSE(HasFeedback(graph));
}
