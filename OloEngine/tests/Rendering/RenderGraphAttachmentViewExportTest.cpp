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
//   * a temporal history extracted from a view keeps the framebuffer alive
//     to the end of the frame, so the end-of-frame copy cannot read storage
//     the transient planner already handed to another resource.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

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
