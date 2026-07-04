#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "PropertyTests/RenderPropertyTest.h"
#include "TestDeclarativeNode.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Passes/ChromaticAberrationRenderPass.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"
#include "OloEngine/Renderer/Passes/FXAARenderPass.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/Passes/MotionBlurRenderPass.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/VignetteRenderPass.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Minimal Stub RenderPass for Graph Tests (no GL)
// =============================================================================

class StubRenderPass : public RenderGraphNode
{
  public:
    explicit StubRenderPass(const std::string& name)
    {
        m_Name = name;
    }
    ~StubRenderPass() override = default;

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute(RGCommandContext& /*context*/) override
    {
        ++m_ExecuteCount;
    }
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }
    void SetupFramebuffer(u32 /*w*/, u32 /*h*/) override {}
    void ResizeFramebuffer(u32 /*w*/, u32 /*h*/) override {}
    void OnReset() override {}

    u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

  private:
    u32 m_ExecuteCount = 0;
};

class CallbackStyleStubPass : public RenderGraphNode
{
  public:
    using SetupFn = std::function<void(RGBuilder&, FrameBlackboard&)>;

    explicit CallbackStyleStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}

    void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override
    {
        RenderGraphNode::Setup(builder, blackboard);
        if (m_Setup)
            m_Setup(builder, blackboard);
    }

    void Execute(RGCommandContext& /*context*/) override {}

    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    void SetSetupBehavior(SetupFn setup)
    {
        m_Setup = std::move(setup);
    }

  private:
    SetupFn m_Setup;
};

class BucketStubPass : public CommandBufferRenderPass
{
  public:
    explicit BucketStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute(RGCommandContext& /*context*/) override {}
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }
};

class ImmediateStubPass : public TestDeclarativeNode
{
  public:
    explicit ImmediateStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute(RGCommandContext& /*context*/) override {}
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    void TestDeclareRead(std::string_view resource)
    {
        DeclareTestRead(resource, ResourceHandle::Kind::Texture2D);
    }
};

class LifecycleTrackingPass : public RenderGraphNode
{
  public:
    explicit LifecycleTrackingPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute(RGCommandContext& /*context*/) override {}
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    void ResizeFramebuffer(u32 width, u32 height) override
    {
        ++m_ResizeFramebufferCount;
        m_LastResizeFramebufferWidth = width;
        m_LastResizeFramebufferHeight = height;
    }

    void ApplyRenderViewport(u32 width, u32 height) override
    {
        ++m_ApplyRenderViewportCount;
        m_LastRenderViewportWidth = width;
        m_LastRenderViewportHeight = height;
    }

    [[nodiscard]] u32 GetResizeFramebufferCount() const
    {
        return m_ResizeFramebufferCount;
    }

    [[nodiscard]] u32 GetApplyRenderViewportCount() const
    {
        return m_ApplyRenderViewportCount;
    }

    [[nodiscard]] u32 GetLastResizeFramebufferWidth() const
    {
        return m_LastResizeFramebufferWidth;
    }

    [[nodiscard]] u32 GetLastResizeFramebufferHeight() const
    {
        return m_LastResizeFramebufferHeight;
    }

    [[nodiscard]] u32 GetLastRenderViewportWidth() const
    {
        return m_LastRenderViewportWidth;
    }

    [[nodiscard]] u32 GetLastRenderViewportHeight() const
    {
        return m_LastRenderViewportHeight;
    }

  private:
    u32 m_ResizeFramebufferCount = 0;
    u32 m_ApplyRenderViewportCount = 0;
    u32 m_LastResizeFramebufferWidth = 0;
    u32 m_LastResizeFramebufferHeight = 0;
    u32 m_LastRenderViewportWidth = 0;
    u32 m_LastRenderViewportHeight = 0;
};

class StubFramebuffer : public Framebuffer
{
  public:
    explicit StubFramebuffer(u32 rendererID)
        : m_RendererID(rendererID) {}

    void Bind() override {}
    void Unbind() override {}
    void Resize(u32 width, u32 height) override
    {
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_RenderViewportWidth = 0;
        m_RenderViewportHeight = 0;
    }
    void SetRenderViewportSize(u32 width, u32 height) override
    {
        m_RenderViewportWidth = width;
        m_RenderViewportHeight = height;
    }
    [[nodiscard]] u32 GetRenderViewportWidth() const override
    {
        return m_RenderViewportWidth;
    }
    [[nodiscard]] u32 GetRenderViewportHeight() const override
    {
        return m_RenderViewportHeight;
    }
    int ReadPixel(u32 /*attachmentIndex*/, int /*x*/, int /*y*/) override
    {
        return 0;
    }
    void ClearAttachment(u32 /*attachmentIndex*/, int /*value*/) override {}
    void ClearAttachment(u32 /*attachmentIndex*/, const glm::vec4& /*value*/) override {}
    void ClearAllAttachments(const glm::vec4& /*clearColor*/, int /*entityIdClear*/) override {}
    [[nodiscard]] u32 GetColorAttachmentRendererID(u32 /*index*/) const override
    {
        return 0;
    }
    [[nodiscard]] u32 GetDepthAttachmentRendererID() const override
    {
        return 0;
    }
    [[nodiscard]] const FramebufferSpecification& GetSpecification() const override
    {
        return m_Specification;
    }
    [[nodiscard]] u32 GetRendererID() const override
    {
        return m_RendererID;
    }
    void AttachDepthTextureArrayLayer(u32 /*textureArrayRendererID*/, u32 /*layer*/) override {}

  private:
    u32 m_RendererID = 0;
    u32 m_RenderViewportWidth = 0;
    u32 m_RenderViewportHeight = 0;
    FramebufferSpecification m_Specification;
};

class AttachmentStubFramebuffer : public StubFramebuffer
{
  public:
    AttachmentStubFramebuffer(u32 rendererID, u32 colorAttachment0ID)
        : StubFramebuffer(rendererID), m_ColorAttachment0ID(colorAttachment0ID)
    {
    }

    [[nodiscard]] u32 GetColorAttachmentRendererID(u32 index) const override
    {
        return index == 0 ? m_ColorAttachment0ID : 0u;
    }

  private:
    u32 m_ColorAttachment0ID = 0;
};

class MultiAttachmentStubFramebuffer : public StubFramebuffer
{
  public:
    MultiAttachmentStubFramebuffer(u32 rendererID, std::initializer_list<u32> colorAttachmentIDs, u32 depthAttachmentID = 0u)
        : StubFramebuffer(rendererID), m_ColorAttachmentIDs(colorAttachmentIDs), m_DepthAttachmentID(depthAttachmentID)
    {
    }

    [[nodiscard]] u32 GetColorAttachmentRendererID(u32 index) const override
    {
        return index < m_ColorAttachmentIDs.size() ? m_ColorAttachmentIDs[index] : 0u;
    }

    [[nodiscard]] u32 GetDepthAttachmentRendererID() const override
    {
        return m_DepthAttachmentID;
    }

  private:
    std::vector<u32> m_ColorAttachmentIDs;
    u32 m_DepthAttachmentID = 0u;
};

class ContextAwareStubPass : public RenderGraphNode
{
  public:
    explicit ContextAwareStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}

    void Execute(RGCommandContext& context) override
    {
        ++m_ContextExecuteCount;
        m_ContextWasActive = context.IsPassActive();
        m_ContextPassName = std::string(context.GetActivePassName());
    }

    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    [[nodiscard]] u32 GetContextExecuteCount() const
    {
        return m_ContextExecuteCount;
    }

    [[nodiscard]] bool WasContextActive() const
    {
        return m_ContextWasActive;
    }

    [[nodiscard]] const std::string& GetObservedContextPassName() const
    {
        return m_ContextPassName;
    }

  private:
    u32 m_ContextExecuteCount = 0;
    bool m_ContextWasActive = false;
    std::string m_ContextPassName;
};

class InputTrackingStubPass : public RenderGraphNode
{
  public:
    explicit InputTrackingStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute(RGCommandContext& /*context*/) override
    {
        ++m_ExecuteCount;
    }

    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    [[nodiscard]] u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

  private:
    u32 m_ExecuteCount = 0;
};

class ResolveFailureProbePass : public RenderGraphNode
{
  public:
    explicit ResolveFailureProbePass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}

    void Execute(RGCommandContext& context) override
    {
        if (m_TextureHandle.IsValid())
            m_LastResolvedTexture = context.ResolveTexture(m_TextureHandle);
        else
            m_LastResolvedTexture = context.ResolveTexture({});

        m_LastResolvedFramebuffer = context.ResolveFramebuffer({});
    }

    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    void SetTextureHandle(const RGTextureHandle handle)
    {
        m_TextureHandle = handle;
    }

    [[nodiscard]] u32 GetLastResolvedTexture() const
    {
        return m_LastResolvedTexture;
    }

    [[nodiscard]] bool LastResolvedFramebufferWasNull() const
    {
        return m_LastResolvedFramebuffer == nullptr;
    }

  private:
    RGTextureHandle m_TextureHandle{};
    u32 m_LastResolvedTexture = 0;
    Ref<Framebuffer> m_LastResolvedFramebuffer = nullptr;
};

class DeclarationTrackingStubPass : public TestDeclarativeNode
{
  public:
    explicit DeclarationTrackingStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute(RGCommandContext& /*context*/) override
    {
        ++m_ExecuteCount;
    }

    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    void TestDeclareRead(std::string_view resource)
    {
        DeclareTestRead(resource, ResourceHandle::Kind::Framebuffer);
    }

    void TestDeclareWrite(std::string_view resource)
    {
        DeclareTestWrite(resource, ResourceHandle::Kind::Framebuffer);
    }

    [[nodiscard]] u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

  private:
    u32 m_ExecuteCount = 0;
};

class TestGraphNode : public RenderGraphNode
{
  public:
    using SetupFn = std::function<void(RGBuilder&)>;
    using ExecuteFn = std::function<void(RGCommandContext&)>;

    explicit TestGraphNode(std::string name, SetupFn setup = {}, ExecuteFn execute = {})
        : m_Name(std::move(name)), m_Setup(std::move(setup)), m_Execute(std::move(execute))
    {
    }

    [[nodiscard]] const std::string& GetName() const override
    {
        return m_Name;
    }

    void Setup(RGBuilder& builder, FrameBlackboard& /*blackboard*/) override
    {
        ++m_SetupCount;
        // Flush stored DeclareRead/DeclareWrite entries through the setup-time
        // path so tests that record declarations via these helpers exercise the
        // same `m_PassAccessDeclarations` path production passes use.
        for (const auto& read : m_Reads)
            MirrorRead(builder, read);
        for (const auto& write : m_Writes)
            MirrorWrite(builder, write);
        if (m_Setup)
            m_Setup(builder);
    }

    void Execute(RGCommandContext& context) override
    {
        ++m_ExecuteCount;
        m_ContextWasActive = context.IsPassActive();
        m_ContextPassName = std::string(context.GetActivePassName());
        if (m_Execute)
            m_Execute(context);
    }

    [[nodiscard]] RenderGraphNodeFlags GetFlags() const override
    {
        return m_Flags;
    }

    void SetupFramebuffer(u32 width, u32 height) override
    {
        ++m_SetupFramebufferCount;
        m_LastSetupFramebufferWidth = width;
        m_LastSetupFramebufferHeight = height;
    }

    void ResizeFramebuffer(u32 width, u32 height) override
    {
        ++m_ResizeFramebufferCount;
        m_LastResizeFramebufferWidth = width;
        m_LastResizeFramebufferHeight = height;
    }

    void ApplyRenderViewport(u32 width, u32 height) override
    {
        ++m_ApplyRenderViewportCount;
        m_LastRenderViewportWidth = width;
        m_LastRenderViewportHeight = height;
    }

    void SetFlags(RenderGraphNodeFlags flags)
    {
        m_Flags = flags;
    }

    void DeclareRead(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
    {
        if (const auto it = std::ranges::find_if(m_Reads,
                                                 [name](const ResourceHandle& handle)
                                                 {
                                                     return handle.Name == name;
                                                 });
            it != m_Reads.end())
        {
            return;
        }

        m_Reads.emplace_back(name, kind);
    }

    void DeclareWrite(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
    {
        if (const auto it = std::ranges::find_if(m_Writes,
                                                 [name](const ResourceHandle& handle)
                                                 {
                                                     return handle.Name == name;
                                                 });
            it != m_Writes.end())
        {
            return;
        }

        m_Writes.emplace_back(name, kind);
    }

    [[nodiscard]] u32 GetSetupCount() const
    {
        return m_SetupCount;
    }

    [[nodiscard]] u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

    [[nodiscard]] bool WasContextActive() const
    {
        return m_ContextWasActive;
    }

    [[nodiscard]] const std::string& GetObservedContextPassName() const
    {
        return m_ContextPassName;
    }

    [[nodiscard]] u32 GetSetupFramebufferCount() const
    {
        return m_SetupFramebufferCount;
    }

    [[nodiscard]] u32 GetResizeFramebufferCount() const
    {
        return m_ResizeFramebufferCount;
    }

    [[nodiscard]] u32 GetApplyRenderViewportCount() const
    {
        return m_ApplyRenderViewportCount;
    }

    [[nodiscard]] u32 GetLastSetupFramebufferWidth() const
    {
        return m_LastSetupFramebufferWidth;
    }

    [[nodiscard]] u32 GetLastSetupFramebufferHeight() const
    {
        return m_LastSetupFramebufferHeight;
    }

    [[nodiscard]] u32 GetLastResizeFramebufferWidth() const
    {
        return m_LastResizeFramebufferWidth;
    }

    [[nodiscard]] u32 GetLastResizeFramebufferHeight() const
    {
        return m_LastResizeFramebufferHeight;
    }

    [[nodiscard]] u32 GetLastRenderViewportWidth() const
    {
        return m_LastRenderViewportWidth;
    }

    [[nodiscard]] u32 GetLastRenderViewportHeight() const
    {
        return m_LastRenderViewportHeight;
    }

  private:
    std::string m_Name;
    SetupFn m_Setup;
    ExecuteFn m_Execute;
    RenderGraphNodeFlags m_Flags = RenderGraphNodeFlags::Graphics;
    std::vector<ResourceHandle> m_Reads;
    std::vector<ResourceHandle> m_Writes;
    u32 m_SetupCount = 0;
    u32 m_ExecuteCount = 0;
    u32 m_SetupFramebufferCount = 0;
    u32 m_ResizeFramebufferCount = 0;
    u32 m_ApplyRenderViewportCount = 0;
    u32 m_LastSetupFramebufferWidth = 0;
    u32 m_LastSetupFramebufferHeight = 0;
    u32 m_LastResizeFramebufferWidth = 0;
    u32 m_LastResizeFramebufferHeight = 0;
    u32 m_LastRenderViewportWidth = 0;
    u32 m_LastRenderViewportHeight = 0;
    bool m_ContextWasActive = false;
    std::string m_ContextPassName;

    static ResourceHandle::Kind NormalizeKind(ResourceHandle::Kind kind)
    {
        return kind == ResourceHandle::Kind::Unknown ? ResourceHandle::Kind::Texture2D : kind;
    }

    static RGResourceDesc MakeMirrorDesc(ResourceHandle::Kind kind, std::string_view name)
    {
        auto desc = RGResourceDesc::FromHandleKind(kind, name);
        desc.Imported = false;
        return desc;
    }

    static void MirrorRead(RGBuilder& builder, const ResourceHandle& resource)
    {
        const auto kind = NormalizeKind(resource.Type);
        const auto desc = MakeMirrorDesc(kind, resource.Name);
        switch (kind)
        {
            case ResourceHandle::Kind::Framebuffer:
            {
                auto handle = builder.CreateFramebuffer(resource.Name, desc);
                [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::RenderTargetRead);
                break;
            }
            case ResourceHandle::Kind::UniformBuffer:
            case ResourceHandle::Kind::StorageBuffer:
            {
                auto handle = builder.CreateBuffer(resource.Name, desc);
                [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderStorage);
                break;
            }
            default:
            {
                auto handle = builder.CreateTexture(resource.Name, desc);
                [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderSample);
                break;
            }
        }
    }

    static void MirrorWrite(RGBuilder& builder, const ResourceHandle& resource)
    {
        const auto kind = NormalizeKind(resource.Type);
        const auto desc = MakeMirrorDesc(kind, resource.Name);
        switch (kind)
        {
            case ResourceHandle::Kind::Framebuffer:
            {
                auto handle = builder.CreateFramebuffer(resource.Name, desc);
                builder.Write(handle, RGWriteUsage::RenderTarget);
                break;
            }
            case ResourceHandle::Kind::UniformBuffer:
            case ResourceHandle::Kind::StorageBuffer:
            {
                auto handle = builder.CreateBuffer(resource.Name, desc);
                builder.Write(handle, RGWriteUsage::ShaderStorage);
                break;
            }
            default:
            {
                auto handle = builder.CreateTexture(resource.Name, desc);
                builder.Write(handle, RGWriteUsage::RenderTarget);
                break;
            }
        }
    }
};

template<typename TPass>
static Ref<TPass> AddPassNode(RenderGraph& graph,
                              Ref<TPass> pass)
{
    OLO_CORE_ASSERT(pass, "AddPassNode requires a valid pass");
    OLO_CORE_ASSERT(!pass->GetName().empty(), "AddPassNode requires a named pass");

    graph.AddNode(pass.template As<RenderGraphNode>());

    return pass;
}

// Helper to create and add a stub pass through the graph-native node path.
static Ref<StubRenderPass> AddStub(RenderGraph& graph, const std::string& name)
{
    auto pass = Ref<StubRenderPass>::Create(name);
    pass->SetName(name);
    AddPassNode(graph, pass);
    return pass;
}

static Ref<TestGraphNode> AddTestNode(RenderGraph& graph,
                                      std::string name,
                                      TestGraphNode::SetupFn setup = {},
                                      TestGraphNode::ExecuteFn execute = {})
{
    auto node = Ref<TestGraphNode>::Create(std::move(name), std::move(setup), std::move(execute));
    graph.AddNode(node);
    return node;
}

static Ref<TestGraphNode> AddSetupNode(RenderGraph& graph, std::string name, TestGraphNode::SetupFn setup)
{
    return AddTestNode(graph, std::move(name), std::move(setup));
}

static Ref<TestGraphNode> AddSetupNode(RenderGraph& graph,
                                       std::string name,
                                       RenderGraphNodeFlags flags,
                                       TestGraphNode::SetupFn setup)
{
    auto node = AddTestNode(graph, std::move(name), std::move(setup));
    node->SetFlags(flags);
    return node;
}

// =============================================================================
// Basic Graph Construction
// =============================================================================

TEST(RenderGraph, PassNodesAreRetrievableAsRenderPasses)
{
    RenderGraph graph;

    auto pass = AddStub(graph, "PassA");
    auto retrieved = graph.GetNode<RenderGraphNode>("PassA");

    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->GetName(), "PassA");
    EXPECT_EQ(retrieved.Raw(), pass.As<RenderGraphNode>().Raw());
}

TEST(RenderGraph, GetNodeReturnsNullForUnknown)
{
    RenderGraph graph;
    auto node = graph.GetNode<RenderGraphNode>("NonExistent");
    EXPECT_EQ(node, nullptr);
}

TEST(RenderGraph, GetNodeSubmissionInfoReturnsAllRegisteredEntries)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    const auto all = graph.GetNodeSubmissionInfo();
    EXPECT_EQ(all.size(), 3u);
}

TEST(RenderGraph, NodeSubmissionInfoReportsResourceDeclarations)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto bucket = Ref<BucketStubPass>::Create("BucketPass");
    auto immediate = Ref<ImmediateStubPass>::Create("ImmediatePass");
    immediate->TestDeclareRead("SceneColor");

    AddPassNode(graph, bucket);
    AddPassNode(graph, immediate);

    // Declarations are flushed through the setup-time path; resource
    // visibility comes from compiled state after BuildFrameGraph.
    graph.SetFinalPass("ImmediatePass");
    graph.BuildFrameGraph();

    const auto info = graph.GetNodeSubmissionInfo();
    ASSERT_EQ(info.size(), 2u);

    const auto bucketIt = std::ranges::find_if(info, [](const RenderGraph::NodeSubmissionInfo& entry)
                                               { return entry.NodeName == "BucketPass"; });
    ASSERT_NE(bucketIt, info.end());
    EXPECT_FALSE(bucketIt->DeclaresResources);

    const auto immediateIt = std::ranges::find_if(info, [](const RenderGraph::NodeSubmissionInfo& entry)
                                                  { return entry.NodeName == "ImmediatePass"; });
    ASSERT_NE(immediateIt, info.end());
    EXPECT_TRUE(immediateIt->DeclaresResources);
}

TEST(RenderGraph, GraphNodeStaticDeclarationsPopulateRegistryAndSubmissionInfo)
{
    RenderGraph graph;

    auto producer = AddTestNode(graph, "GraphProducer");
    auto final = AddTestNode(graph, "GraphFinal");

    producer->DeclareWrite("GraphNodeColor", ResourceHandle::Kind::Texture2D);
    final->DeclareRead("GraphNodeColor", ResourceHandle::Kind::Texture2D);

    graph.SetFinalPass("GraphFinal");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty());

    const auto submissionInfo = graph.GetNodeSubmissionInfo();
    const auto producerIt = std::ranges::find_if(submissionInfo,
                                                 [](const RenderGraph::NodeSubmissionInfo& entry)
                                                 {
                                                     return entry.NodeName == "GraphProducer";
                                                 });
    ASSERT_NE(producerIt, submissionInfo.end());
    EXPECT_TRUE(producerIt->DeclaresResources);

    const auto finalIt = std::ranges::find_if(submissionInfo,
                                              [](const RenderGraph::NodeSubmissionInfo& entry)
                                              {
                                                  return entry.NodeName == "GraphFinal";
                                              });
    ASSERT_NE(finalIt, submissionInfo.end());
    EXPECT_TRUE(finalIt->DeclaresResources);

    const auto* resource = graph.FindRegisteredResource("GraphNodeColor");
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->Desc.Kind, ResourceHandle::Kind::Texture2D);
    ASSERT_EQ(resource->Producers.size(), 1u);
    EXPECT_EQ(resource->Producers[0], "GraphProducer");
    ASSERT_EQ(resource->Consumers.size(), 1u);
    EXPECT_EQ(resource->Consumers[0], "GraphFinal");
}

TEST(RenderGraph, ValidateExecutionTopologyReturnsFalseForCycles)
{
    RenderGraph graph;

    AddStub(graph, "PassA");
    AddStub(graph, "PassB");

    graph.ConnectPass("PassA", "PassB");
    graph.ConnectPass("PassB", "PassA");
    graph.SetFinalPass("PassB");

    EXPECT_FALSE(graph.ValidateExecutionTopology())
        << "ValidateExecutionTopology should reject cyclic graphs without relying on resource declarations.";
}

TEST(RenderGraph, CompiledHazardValidationReportsDynamicFeedbackHazards)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "FeedbackPass",
        [](RGBuilder& builder)
        {
            auto feedbackTexture = builder.ImportTexture(
                "FeedbackTex",
                5,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FeedbackTex"));
            [[maybe_unused]] const auto readFeedback = builder.Read(feedbackTexture, RGReadUsage::ShaderSample);
            builder.Write(feedbackTexture, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("FeedbackPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateCompiledResourceHazards();
    const bool hasFeedbackHazard = std::ranges::any_of(
        hazards,
        [](const RenderGraph::Hazard& hazard)
        {
            return hazard.Kind == RenderGraph::HazardKind::FeedbackWithoutDeclaration &&
                   hazard.Resource == "FeedbackTex" && hazard.Consumer == "FeedbackPass";
        });

    EXPECT_TRUE(hasFeedbackHazard)
        << "Compiled-frame validation must report same-pass dynamic read/write feedback hazards";
}

TEST(RenderGraph, ExecuteProvidesActiveCommandContextScope)
{
    RenderGraph graph;

    auto pass = Ref<ContextAwareStubPass>::Create("ContextPass");
    AddPassNode(graph, pass);

    graph.Execute();

    EXPECT_EQ(pass->GetContextExecuteCount(), 1u);
    EXPECT_TRUE(pass->WasContextActive());
    EXPECT_EQ(pass->GetObservedContextPassName(), "ContextPass");
}

TEST(RenderGraph, SetupSelectedPrimaryInputFramebufferTracksCurrentFirstValidHandle)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto firstFramebuffer = Ref<AttachmentStubFramebuffer>::Create(101u, 201u);
    auto secondFramebuffer = Ref<AttachmentStubFramebuffer>::Create(102u, 202u);

    const auto firstHandle = graph.ImportFramebuffer("SelectedInputA", firstFramebuffer);
    const auto secondHandle = graph.ImportFramebuffer("SelectedInputB", secondFramebuffer);

    bool useFirstHandle = true;
    bool useSecondHandle = true;
    bool skipSelection = false;

    auto pass = Ref<CallbackStyleStubPass>::Create("SelectedInputPass");
    pass->SetSetupBehavior(
        [passRaw = pass.Raw(), firstHandle, secondHandle, &useFirstHandle, &useSecondHandle, &skipSelection](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            if (skipSelection)
                return;

            RenderPipelineBuilderInternal::ReadFirstValidFramebufferForPass(
                builder,
                passRaw,
                useFirstHandle ? firstHandle : RGFramebufferHandle{},
                useSecondHandle ? secondHandle : RGFramebufferHandle{});
        });
    AddPassNode(graph, pass);

    graph.SetFinalPass("SelectedInputPass");

    graph.BuildFrameGraph();
    EXPECT_EQ(pass->GetPrimaryInputFramebufferHandle(), firstHandle);
    EXPECT_EQ(graph.ResolveFramebuffer(pass->GetPrimaryInputFramebufferHandle()).Raw(), firstFramebuffer.Raw());

    useFirstHandle = false;
    graph.BuildFrameGraph();
    EXPECT_EQ(pass->GetPrimaryInputFramebufferHandle(), secondHandle);
    EXPECT_EQ(graph.ResolveFramebuffer(pass->GetPrimaryInputFramebufferHandle()).Raw(), secondFramebuffer.Raw());

    skipSelection = true;
    graph.BuildFrameGraph();
    EXPECT_FALSE(pass->GetPrimaryInputFramebufferHandle().IsValid());
}

TEST(RenderGraph, SetupSelectedPrimaryInputFramebufferTextureTracksCurrentFirstValidHandlePair)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto firstFramebuffer = Ref<AttachmentStubFramebuffer>::Create(111u, 211u);
    auto secondFramebuffer = Ref<AttachmentStubFramebuffer>::Create(112u, 212u);

    auto makeInputDesc = [](std::string_view name)
    {
        auto desc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, name);
        desc.Format = RGResourceFormat::RGBA16Float;
        desc.Width = 1u;
        desc.Height = 1u;
        return desc;
    };

    const auto firstHandle = graph.ImportFramebuffer("SelectedPairedInputA", firstFramebuffer, makeInputDesc("SelectedPairedInputA"));
    const auto secondHandle = graph.ImportFramebuffer("SelectedPairedInputB", secondFramebuffer, makeInputDesc("SelectedPairedInputB"));
    const auto firstTexture = graph.CreateFramebufferAttachmentView("SelectedPairedInputATexture", firstHandle, 0u);
    const auto secondTexture = graph.CreateFramebufferAttachmentView("SelectedPairedInputBTexture", secondHandle, 0u);

    bool useFirstHandle = true;
    bool useSecondHandle = true;
    bool skipSelection = false;

    auto pass = Ref<CallbackStyleStubPass>::Create("SelectedPairedInputPass");
    pass->SetSetupBehavior(
        [passRaw = pass.Raw(), firstHandle, secondHandle, firstTexture, secondTexture, &useFirstHandle, &useSecondHandle, &skipSelection](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            if (skipSelection)
                return;

            [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidFramebufferTextureInputForPass(
                builder,
                passRaw,
                RenderPipelineBuilderInternal::MakeFramebufferTextureInput(
                    useFirstHandle ? firstHandle : RGFramebufferHandle{},
                    useFirstHandle ? firstTexture : RGTextureHandle{}),
                RenderPipelineBuilderInternal::MakeFramebufferTextureInput(
                    useSecondHandle ? secondHandle : RGFramebufferHandle{},
                    useSecondHandle ? secondTexture : RGTextureHandle{}));
        });
    AddPassNode(graph, pass);

    graph.SetFinalPass("SelectedPairedInputPass");

    graph.BuildFrameGraph();
    EXPECT_EQ(pass->GetPrimaryInputFramebufferHandle(), firstHandle);
    EXPECT_EQ(pass->GetPrimaryInputTextureHandle(), firstTexture);
    EXPECT_EQ(graph.ResolveFramebuffer(pass->GetPrimaryInputFramebufferHandle()).Raw(), firstFramebuffer.Raw());
    EXPECT_EQ(graph.ResolveTexture(pass->GetPrimaryInputTextureHandle()), 211u);

    useFirstHandle = false;
    graph.BuildFrameGraph();
    EXPECT_EQ(pass->GetPrimaryInputFramebufferHandle(), secondHandle);
    EXPECT_EQ(pass->GetPrimaryInputTextureHandle(), secondTexture);
    EXPECT_EQ(graph.ResolveFramebuffer(pass->GetPrimaryInputFramebufferHandle()).Raw(), secondFramebuffer.Raw());
    EXPECT_EQ(graph.ResolveTexture(pass->GetPrimaryInputTextureHandle()), 212u);

    skipSelection = true;
    graph.BuildFrameGraph();
    EXPECT_FALSE(pass->GetPrimaryInputFramebufferHandle().IsValid());
    EXPECT_FALSE(pass->GetPrimaryInputTextureHandle().IsValid());
}

TEST(RenderGraph, SetupSelectedPrimaryOutputFramebufferTracksCurrentHandleAndResetsBetweenFrames)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto firstFramebuffer = Ref<AttachmentStubFramebuffer>::Create(121u, 221u);
    auto secondFramebuffer = Ref<AttachmentStubFramebuffer>::Create(122u, 222u);

    const auto firstHandle = graph.ImportFramebuffer("SelectedOutputA", firstFramebuffer);
    const auto secondHandle = graph.ImportFramebuffer("SelectedOutputB", secondFramebuffer);

    bool useFirstHandle = true;
    bool skipSelection = false;

    auto pass = Ref<CallbackStyleStubPass>::Create("SelectedOutputPass");
    pass->SetSetupBehavior(
        [passRaw = pass.Raw(), firstHandle, secondHandle, &useFirstHandle, &skipSelection](RGBuilder& /*builder*/, FrameBlackboard& /*blackboard*/)
        {
            if (skipSelection)
                return;

            passRaw->SetPrimaryOutputFramebufferHandle(useFirstHandle ? firstHandle : secondHandle);
        });
    AddPassNode(graph, pass);

    graph.SetFinalPass("SelectedOutputPass");

    graph.BuildFrameGraph();
    EXPECT_EQ(pass->GetPrimaryOutputFramebufferHandle(), firstHandle);
    EXPECT_EQ(graph.ResolveFramebuffer(pass->GetPrimaryOutputFramebufferHandle()).Raw(), firstFramebuffer.Raw());

    useFirstHandle = false;
    graph.BuildFrameGraph();
    EXPECT_EQ(pass->GetPrimaryOutputFramebufferHandle(), secondHandle);
    EXPECT_EQ(graph.ResolveFramebuffer(pass->GetPrimaryOutputFramebufferHandle()).Raw(), secondFramebuffer.Raw());

    skipSelection = true;
    graph.BuildFrameGraph();
    EXPECT_FALSE(pass->GetPrimaryOutputFramebufferHandle().IsValid());
}

TEST(RenderGraph, UICompositePublishesVersionedOutputAndFinalPrefersProducerOwnedVersion)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto sceneFramebuffer = Ref<AttachmentStubFramebuffer>::Create(301u, 401u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(302u, 402u);

    const auto sceneHandle = graph.ImportFramebuffer(ResourceNames::SceneColor,
                                                     sceneFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::SceneColor, RGResourceFormat::RGBA16Float));
    const auto sceneTexture = graph.CreateFramebufferAttachmentView(ResourceNames::SceneColorTexture, sceneHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Scene.SceneColor = sceneHandle;
    blackboard.Scene.SceneColorTexture = sceneTexture;
    blackboard.Post.PostProcessColor = sceneHandle;
    blackboard.Post.PostProcessColorTexture = sceneTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;
    blackboard.Post.Backbuffer = graph.ImportFramebuffer(
        ResourceNames::Backbuffer,
        nullptr,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, ResourceNames::Backbuffer));

    auto uiComposite = Ref<UICompositeRenderPass>::Create();
    uiComposite->SetName("UICompositePass");
    auto finalPass = Ref<FinalRenderPass>::Create();
    finalPass->SetName("FinalPass");

    AddPassNode(graph, uiComposite);
    AddPassNode(graph, finalPass);
    graph.SetFinalPass("FinalPass");

    graph.BuildFrameGraph();

    const auto versionedOutput = uiComposite->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = uiComposite->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "UIComposite@UICompositePass");
    EXPECT_EQ(finalPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(finalPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::UIComposite), versionedOutput)
        << "Canonical UIComposite lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("UIComposite@UICompositePass"), versionedOutput)
        << "Exact versioned UIComposite lookup should remain valid";
}

TEST(RenderGraph, FXAAPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersion)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto vignetteFramebuffer = Ref<AttachmentStubFramebuffer>::Create(311u, 411u);
    auto fxaaFramebuffer = Ref<AttachmentStubFramebuffer>::Create(312u, 412u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(313u, 413u);

    const auto vignetteHandle = graph.ImportFramebuffer(ResourceNames::VignetteColor,
                                                        vignetteFramebuffer,
                                                        makeFramebufferDesc(ResourceNames::VignetteColor, RGResourceFormat::RGBA8UNorm));
    const auto vignetteTexture = graph.CreateFramebufferAttachmentView(ResourceNames::VignetteColorTexture, vignetteHandle, 0u);

    const auto fxaaHandle = graph.ImportFramebuffer(ResourceNames::FXAAColor,
                                                    fxaaFramebuffer,
                                                    makeFramebufferDesc(ResourceNames::FXAAColor, RGResourceFormat::RGBA8UNorm));
    const auto fxaaTexture = graph.CreateFramebufferAttachmentView(ResourceNames::FXAAColorTexture, fxaaHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.VignetteColor = vignetteHandle;
    blackboard.Post.VignetteColorTexture = vignetteTexture;
    blackboard.Post.FXAAColor = fxaaHandle;
    blackboard.Post.FXAAColorTexture = fxaaTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto fxaaPass = Ref<FXAARenderPass>::Create();
    fxaaPass->SetName("FXAAPass");
    fxaaPass->SetEnabled(true);

    auto selectionOutlinePass = Ref<SelectionOutlineRenderPass>::Create();
    selectionOutlinePass->SetName("SelectionOutlinePass");

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, fxaaPass);
    AddPassNode(graph, selectionOutlinePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = fxaaPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = fxaaPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "FXAAColor@FXAAPass");
    EXPECT_EQ(selectionOutlinePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(selectionOutlinePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::FXAAColor), versionedOutput)
        << "Canonical FXAAColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("FXAAColor@FXAAPass"), versionedOutput)
        << "Exact versioned FXAAColor lookup should remain valid";
}

TEST(RenderGraph, VignettePublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenFXAAIsAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto toneMapFramebuffer = Ref<AttachmentStubFramebuffer>::Create(321u, 421u);
    auto vignetteFramebuffer = Ref<AttachmentStubFramebuffer>::Create(322u, 422u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(323u, 423u);

    const auto toneMapHandle = graph.ImportFramebuffer(ResourceNames::ToneMapColor,
                                                       toneMapFramebuffer,
                                                       makeFramebufferDesc(ResourceNames::ToneMapColor, RGResourceFormat::RGBA8UNorm));
    const auto toneMapTexture = graph.CreateFramebufferAttachmentView(ResourceNames::ToneMapColorTexture, toneMapHandle, 0u);

    const auto vignetteHandle = graph.ImportFramebuffer(ResourceNames::VignetteColor,
                                                        vignetteFramebuffer,
                                                        makeFramebufferDesc(ResourceNames::VignetteColor, RGResourceFormat::RGBA8UNorm));
    const auto vignetteTexture = graph.CreateFramebufferAttachmentView(ResourceNames::VignetteColorTexture, vignetteHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.ToneMapColor = toneMapHandle;
    blackboard.Post.ToneMapColorTexture = toneMapTexture;
    blackboard.Post.VignetteColor = vignetteHandle;
    blackboard.Post.VignetteColorTexture = vignetteTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(true);

    auto selectionOutlinePass = Ref<SelectionOutlineRenderPass>::Create();
    selectionOutlinePass->SetName("SelectionOutlinePass");

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, vignettePass);
    AddPassNode(graph, selectionOutlinePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = vignettePass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = vignettePass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "VignetteColor@VignettePass");
    EXPECT_EQ(selectionOutlinePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(selectionOutlinePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::VignetteColor), versionedOutput)
        << "Canonical VignetteColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("VignetteColor@VignettePass"), versionedOutput)
        << "Exact versioned VignetteColor lookup should remain valid";
}

TEST(RenderGraph, ToneMapPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto colorGradingFramebuffer = Ref<AttachmentStubFramebuffer>::Create(331u, 431u);
    auto toneMapFramebuffer = Ref<AttachmentStubFramebuffer>::Create(332u, 432u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(333u, 433u);

    const auto colorGradingHandle = graph.ImportFramebuffer(ResourceNames::ColorGradingColor,
                                                            colorGradingFramebuffer,
                                                            makeFramebufferDesc(ResourceNames::ColorGradingColor, RGResourceFormat::RGBA8UNorm));
    const auto colorGradingTexture = graph.CreateFramebufferAttachmentView(ResourceNames::ColorGradingColorTexture, colorGradingHandle, 0u);

    const auto toneMapHandle = graph.ImportFramebuffer(ResourceNames::ToneMapColor,
                                                       toneMapFramebuffer,
                                                       makeFramebufferDesc(ResourceNames::ToneMapColor, RGResourceFormat::RGBA8UNorm));
    const auto toneMapTexture = graph.CreateFramebufferAttachmentView(ResourceNames::ToneMapColorTexture, toneMapHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.ColorGradingColor = colorGradingHandle;
    blackboard.Post.ColorGradingColorTexture = colorGradingTexture;
    blackboard.Post.ToneMapColor = toneMapHandle;
    blackboard.Post.ToneMapColorTexture = toneMapTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = toneMapPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = toneMapPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "ToneMapColor@ToneMapPass");
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::ToneMapColor), versionedOutput)
        << "Canonical ToneMapColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("ToneMapColor@ToneMapPass"), versionedOutput)
        << "Exact versioned ToneMapColor lookup should remain valid";
}

TEST(RenderGraph, ColorGradingPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto chromAbFramebuffer = Ref<AttachmentStubFramebuffer>::Create(341u, 441u);
    auto colorGradingFramebuffer = Ref<AttachmentStubFramebuffer>::Create(342u, 442u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(343u, 443u);

    const auto chromAbHandle = graph.ImportFramebuffer(ResourceNames::ChromAbColor,
                                                       chromAbFramebuffer,
                                                       makeFramebufferDesc(ResourceNames::ChromAbColor, RGResourceFormat::RGBA8UNorm));
    const auto chromAbTexture = graph.CreateFramebufferAttachmentView(ResourceNames::ChromAbColorTexture, chromAbHandle, 0u);

    const auto colorGradingHandle = graph.ImportFramebuffer(ResourceNames::ColorGradingColor,
                                                            colorGradingFramebuffer,
                                                            makeFramebufferDesc(ResourceNames::ColorGradingColor, RGResourceFormat::RGBA8UNorm));
    const auto colorGradingTexture = graph.CreateFramebufferAttachmentView(ResourceNames::ColorGradingColorTexture, colorGradingHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.ChromAbColor = chromAbHandle;
    blackboard.Post.ChromAbColorTexture = chromAbTexture;
    blackboard.Post.ColorGradingColor = colorGradingHandle;
    blackboard.Post.ColorGradingColorTexture = colorGradingTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(true);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = colorGradingPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = colorGradingPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "ColorGradingColor@ColorGradingPass");
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::ColorGradingColor), versionedOutput)
        << "Canonical ColorGradingColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("ColorGradingColor@ColorGradingPass"), versionedOutput)
        << "Exact versioned ColorGradingColor lookup should remain valid";
}

TEST(RenderGraph, ChromaticAberrationPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto fogFramebuffer = Ref<AttachmentStubFramebuffer>::Create(351u, 451u);
    auto chromAbFramebuffer = Ref<AttachmentStubFramebuffer>::Create(352u, 452u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(353u, 453u);

    const auto fogHandle = graph.ImportFramebuffer(ResourceNames::FogColor,
                                                   fogFramebuffer,
                                                   makeFramebufferDesc(ResourceNames::FogColor, RGResourceFormat::RGBA16Float));
    const auto fogTexture = graph.CreateFramebufferAttachmentView(ResourceNames::FogColorTexture, fogHandle, 0u);

    const auto chromAbHandle = graph.ImportFramebuffer(ResourceNames::ChromAbColor,
                                                       chromAbFramebuffer,
                                                       makeFramebufferDesc(ResourceNames::ChromAbColor, RGResourceFormat::RGBA16Float));
    const auto chromAbTexture = graph.CreateFramebufferAttachmentView(ResourceNames::ChromAbColorTexture, chromAbHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.FogColor = fogHandle;
    blackboard.Post.FogColorTexture = fogTexture;
    blackboard.Post.ChromAbColor = chromAbHandle;
    blackboard.Post.ChromAbColorTexture = chromAbTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(true);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = chromAbPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = chromAbPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "ChromAbColor@ChromAberrationPass");
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::ChromAbColor), versionedOutput)
        << "Canonical ChromAbColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("ChromAbColor@ChromAberrationPass"), versionedOutput)
        << "Exact versioned ChromAbColor lookup should remain valid";
}

TEST(RenderGraph, FogPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto precipitationFramebuffer = Ref<AttachmentStubFramebuffer>::Create(361u, 461u);
    auto fogFramebuffer = Ref<AttachmentStubFramebuffer>::Create(362u, 462u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(363u, 463u);

    const auto precipitationHandle = graph.ImportFramebuffer(ResourceNames::PrecipitationColor,
                                                             precipitationFramebuffer,
                                                             makeFramebufferDesc(ResourceNames::PrecipitationColor, RGResourceFormat::RGBA16Float));
    const auto precipitationTexture = graph.CreateFramebufferAttachmentView(ResourceNames::PrecipitationColorTexture, precipitationHandle, 0u);

    const auto fogHandle = graph.ImportFramebuffer(ResourceNames::FogColor,
                                                   fogFramebuffer,
                                                   makeFramebufferDesc(ResourceNames::FogColor, RGResourceFormat::RGBA16Float));
    const auto fogTexture = graph.CreateFramebufferAttachmentView(ResourceNames::FogColorTexture, fogHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.PrecipitationColor = precipitationHandle;
    blackboard.Post.PrecipitationColorTexture = precipitationTexture;
    blackboard.Post.FogColor = fogHandle;
    blackboard.Post.FogColorTexture = fogTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto fogPass = Ref<FogRenderPass>::Create();
    fogPass->SetName("FogPass");
    fogPass->SetEnabled(true);

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(false);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, fogPass);
    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = fogPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = fogPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "FogColor@FogPass");
    EXPECT_EQ(chromAbPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(chromAbPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::FogColor), versionedOutput)
        << "Canonical FogColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("FogColor@FogPass"), versionedOutput)
        << "Exact versioned FogColor lookup should remain valid";
}

TEST(RenderGraph, PrecipitationPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto motionBlurFramebuffer = Ref<AttachmentStubFramebuffer>::Create(371u, 471u);
    auto precipitationFramebuffer = Ref<AttachmentStubFramebuffer>::Create(372u, 472u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(373u, 473u);

    const auto motionBlurHandle = graph.ImportFramebuffer(ResourceNames::MotionBlurColor,
                                                          motionBlurFramebuffer,
                                                          makeFramebufferDesc(ResourceNames::MotionBlurColor, RGResourceFormat::RGBA16Float));
    const auto motionBlurTexture = graph.CreateFramebufferAttachmentView(ResourceNames::MotionBlurColorTexture, motionBlurHandle, 0u);

    const auto precipitationHandle = graph.ImportFramebuffer(ResourceNames::PrecipitationColor,
                                                             precipitationFramebuffer,
                                                             makeFramebufferDesc(ResourceNames::PrecipitationColor, RGResourceFormat::RGBA16Float));
    const auto precipitationTexture = graph.CreateFramebufferAttachmentView(ResourceNames::PrecipitationColorTexture, precipitationHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.MotionBlurColor = motionBlurHandle;
    blackboard.Post.MotionBlurColorTexture = motionBlurTexture;
    blackboard.Post.PrecipitationColor = precipitationHandle;
    blackboard.Post.PrecipitationColorTexture = precipitationTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto precipitationPass = Ref<PrecipitationRenderPass>::Create();
    precipitationPass->SetName("PrecipitationPass");
    precipitationPass->SetEnabled(true);

    auto fogPass = Ref<FogRenderPass>::Create();
    fogPass->SetName("FogPass");
    fogPass->SetEnabled(false);

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(false);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, precipitationPass);
    AddPassNode(graph, fogPass);
    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = precipitationPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = precipitationPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "PrecipitationColor@PrecipitationPass");
    EXPECT_EQ(fogPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(fogPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(chromAbPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(chromAbPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::PrecipitationColor), versionedOutput)
        << "Canonical PrecipitationColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("PrecipitationColor@PrecipitationPass"), versionedOutput)
        << "Exact versioned PrecipitationColor lookup should remain valid";
}

TEST(RenderGraph, TAAPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto motionBlurFramebuffer = Ref<AttachmentStubFramebuffer>::Create(381u, 481u);
    auto taaFramebuffer = Ref<AttachmentStubFramebuffer>::Create(382u, 482u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(383u, 483u);

    const auto motionBlurHandle = graph.ImportFramebuffer(ResourceNames::MotionBlurColor,
                                                          motionBlurFramebuffer,
                                                          makeFramebufferDesc(ResourceNames::MotionBlurColor, RGResourceFormat::RGBA16Float));
    const auto motionBlurTexture = graph.CreateFramebufferAttachmentView(ResourceNames::MotionBlurColorTexture, motionBlurHandle, 0u);

    const auto taaHandle = graph.ImportFramebuffer(ResourceNames::TAAColor,
                                                   taaFramebuffer,
                                                   makeFramebufferDesc(ResourceNames::TAAColor, RGResourceFormat::RGBA16Float));
    const auto taaTexture = graph.CreateFramebufferAttachmentView(ResourceNames::TAAColorTexture, taaHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.MotionBlurColor = motionBlurHandle;
    blackboard.Post.MotionBlurColorTexture = motionBlurTexture;
    blackboard.Post.TAAColor = taaHandle;
    blackboard.Post.TAAColorTexture = taaTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto taaPass = Ref<TAARenderPass>::Create();
    taaPass->SetName("TAAPass");
    taaPass->SetEnabled(true);

    auto precipitationPass = Ref<PrecipitationRenderPass>::Create();
    precipitationPass->SetName("PrecipitationPass");
    precipitationPass->SetEnabled(false);

    auto fogPass = Ref<FogRenderPass>::Create();
    fogPass->SetName("FogPass");
    fogPass->SetEnabled(false);

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(false);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, taaPass);
    AddPassNode(graph, precipitationPass);
    AddPassNode(graph, fogPass);
    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = taaPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = taaPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "TAAColor@TAAPass");
    EXPECT_EQ(precipitationPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(precipitationPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(fogPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(fogPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(chromAbPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(chromAbPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::TAAColor), versionedOutput)
        << "Canonical TAAColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("TAAColor@TAAPass"), versionedOutput)
        << "Exact versioned TAAColor lookup should remain valid";
}

TEST(RenderGraph, SSSPublishesVersionedOutputAndAOApplyAndBloomPreferProducerOwnedVersion)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto sceneFramebuffer = Ref<AttachmentStubFramebuffer>::Create(388u, 488u);
    auto sssFramebuffer = Ref<AttachmentStubFramebuffer>::Create(387u, 487u);
    auto bloomFramebuffer = Ref<AttachmentStubFramebuffer>::Create(390u, 490u);

    const auto sceneHandle = graph.ImportFramebuffer(ResourceNames::SceneColor,
                                                     sceneFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::SceneColor, RGResourceFormat::RGBA16Float));
    const auto sceneTexture = graph.CreateFramebufferAttachmentView(ResourceNames::SceneColorTexture, sceneHandle, 0u);

    const auto sssHandle = graph.ImportFramebuffer(ResourceNames::SSSColor,
                                                   sssFramebuffer,
                                                   makeFramebufferDesc(ResourceNames::SSSColor, RGResourceFormat::RGBA16Float));
    const auto sssTexture = graph.CreateFramebufferAttachmentView(ResourceNames::SSSColorTexture, sssHandle, 0u);

    const auto bloomHandle = graph.ImportFramebuffer(ResourceNames::BloomColor,
                                                     bloomFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::BloomColor, RGResourceFormat::RGBA16Float));
    const auto bloomTexture = graph.CreateFramebufferAttachmentView(ResourceNames::BloomColorTexture, bloomHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Scene.SceneColor = sceneHandle;
    blackboard.Scene.SceneColorTexture = sceneTexture;
    blackboard.Post.SSSColor = sssHandle;
    blackboard.Post.SSSColorTexture = sssTexture;
    blackboard.Post.BloomColor = bloomHandle;
    blackboard.Post.BloomColorTexture = bloomTexture;

    SnowSettings snowSettings;
    snowSettings.Enabled = true;
    snowSettings.SSSBlurEnabled = true;

    auto sssPass = Ref<SSSRenderPass>::Create();
    sssPass->SetName("SSSPass");
    sssPass->SetSettings(snowSettings);

    auto aoApplyPass = Ref<AOApplyRenderPass>::Create();
    aoApplyPass->SetName("AOApplyPass");
    aoApplyPass->SetEnabled(false);

    auto bloomPass = Ref<BloomRenderPass>::Create();
    bloomPass->SetName("BloomPass");
    bloomPass->SetEnabled(false);

    AddPassNode(graph, sssPass);
    AddPassNode(graph, aoApplyPass);
    AddPassNode(graph, bloomPass);
    graph.SetFinalPass("BloomPass");

    graph.BuildFrameGraph();

    const auto versionedOutput = sssPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = sssPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "SSSColor@SSSPass");
    EXPECT_EQ(aoApplyPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(aoApplyPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(bloomPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(bloomPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::SSSColor), versionedOutput)
        << "Canonical SSSColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("SSSColor@SSSPass"), versionedOutput)
        << "Exact versioned SSSColor lookup should remain valid";
}

TEST(RenderGraph, AOApplyPublishesVersionedOutputAndBloomPrefersProducerOwnedVersion)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto sceneFramebuffer = Ref<AttachmentStubFramebuffer>::Create(388u, 488u);
    auto aoApplyFramebuffer = Ref<AttachmentStubFramebuffer>::Create(389u, 489u);
    auto bloomFramebuffer = Ref<AttachmentStubFramebuffer>::Create(390u, 490u);

    const auto sceneHandle = graph.ImportFramebuffer(ResourceNames::SceneColor,
                                                     sceneFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::SceneColor, RGResourceFormat::RGBA16Float));
    const auto sceneTexture = graph.CreateFramebufferAttachmentView(ResourceNames::SceneColorTexture, sceneHandle, 0u);

    const auto aoApplyHandle = graph.ImportFramebuffer(ResourceNames::AOApplyColor,
                                                       aoApplyFramebuffer,
                                                       makeFramebufferDesc(ResourceNames::AOApplyColor, RGResourceFormat::RGBA16Float));
    const auto aoApplyTexture = graph.CreateFramebufferAttachmentView(ResourceNames::AOApplyColorTexture, aoApplyHandle, 0u);

    const auto bloomHandle = graph.ImportFramebuffer(ResourceNames::BloomColor,
                                                     bloomFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::BloomColor, RGResourceFormat::RGBA16Float));
    const auto bloomTexture = graph.CreateFramebufferAttachmentView(ResourceNames::BloomColorTexture, bloomHandle, 0u);

    const auto aoBufferHandle = graph.ImportTexture(
        ResourceNames::AOBuffer,
        42u,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::AOBuffer));
    const auto sceneDepthHandle = graph.ImportTexture(
        ResourceNames::SceneDepth,
        17u,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepth));

    auto& blackboard = graph.GetBlackboard();
    blackboard.Scene.SceneColor = sceneHandle;
    blackboard.Scene.SceneColorTexture = sceneTexture;
    blackboard.Post.AOApplyColor = aoApplyHandle;
    blackboard.Post.AOApplyColorTexture = aoApplyTexture;
    blackboard.Post.BloomColor = bloomHandle;
    blackboard.Post.BloomColorTexture = bloomTexture;
    blackboard.AO.AOBuffer = aoBufferHandle;
    blackboard.Scene.SceneDepth = sceneDepthHandle;

    auto aoApplyPass = Ref<AOApplyRenderPass>::Create();
    aoApplyPass->SetName("AOApplyPass");
    aoApplyPass->SetEnabled(true);

    auto bloomPass = Ref<BloomRenderPass>::Create();
    bloomPass->SetName("BloomPass");
    bloomPass->SetEnabled(false);

    AddPassNode(graph, aoApplyPass);
    AddPassNode(graph, bloomPass);
    graph.SetFinalPass("BloomPass");

    graph.BuildFrameGraph();

    const auto versionedOutput = aoApplyPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = aoApplyPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "AOApplyColor@AOApplyPass");
    EXPECT_EQ(bloomPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(bloomPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::AOApplyColor), versionedOutput)
        << "Canonical AOApplyColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("AOApplyColor@AOApplyPass"), versionedOutput)
        << "Exact versioned AOApplyColor lookup should remain valid";
}

TEST(RenderGraph, BloomPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto bloomFramebuffer = Ref<AttachmentStubFramebuffer>::Create(390u, 490u);
    auto postProcessFramebuffer = Ref<AttachmentStubFramebuffer>::Create(389u, 489u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(393u, 493u);

    const auto bloomHandle = graph.ImportFramebuffer(ResourceNames::BloomColor,
                                                     bloomFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::BloomColor, RGResourceFormat::RGBA16Float));
    const auto bloomTexture = graph.CreateFramebufferAttachmentView(ResourceNames::BloomColorTexture, bloomHandle, 0u);

    const auto postProcessHandle = graph.ImportFramebuffer(ResourceNames::PostProcessColor,
                                                           postProcessFramebuffer,
                                                           makeFramebufferDesc(ResourceNames::PostProcessColor, RGResourceFormat::RGBA16Float));
    const auto postProcessTexture = graph.CreateFramebufferAttachmentView(ResourceNames::PostProcessColorTexture, postProcessHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.BloomColor = bloomHandle;
    blackboard.Post.BloomColorTexture = bloomTexture;
    blackboard.Post.PostProcessColor = postProcessHandle;
    blackboard.Post.PostProcessColorTexture = postProcessTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto bloomPass = Ref<BloomRenderPass>::Create();
    bloomPass->SetName("BloomPass");
    bloomPass->SetEnabled(true);

    auto dofPass = Ref<DOFRenderPass>::Create();
    dofPass->SetName("DOFPass");
    dofPass->SetEnabled(false);

    auto motionBlurPass = Ref<MotionBlurRenderPass>::Create();
    motionBlurPass->SetName("MotionBlurPass");
    motionBlurPass->SetEnabled(false);

    auto taaPass = Ref<TAARenderPass>::Create();
    taaPass->SetName("TAAPass");
    taaPass->SetEnabled(false);

    auto precipitationPass = Ref<PrecipitationRenderPass>::Create();
    precipitationPass->SetName("PrecipitationPass");
    precipitationPass->SetEnabled(false);

    auto fogPass = Ref<FogRenderPass>::Create();
    fogPass->SetName("FogPass");
    fogPass->SetEnabled(false);

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(false);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, bloomPass);
    AddPassNode(graph, dofPass);
    AddPassNode(graph, motionBlurPass);
    AddPassNode(graph, taaPass);
    AddPassNode(graph, precipitationPass);
    AddPassNode(graph, fogPass);
    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = bloomPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = bloomPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "BloomColor@BloomPass");
    EXPECT_EQ(dofPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(dofPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(motionBlurPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(motionBlurPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(taaPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(taaPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(precipitationPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(precipitationPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(fogPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(fogPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(chromAbPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(chromAbPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::BloomColor), versionedOutput)
        << "Canonical BloomColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("BloomColor@BloomPass"), versionedOutput)
        << "Exact versioned BloomColor lookup should remain valid";
}

TEST(RenderGraph, DOFPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto bloomFramebuffer = Ref<AttachmentStubFramebuffer>::Create(390u, 490u);
    auto dofFramebuffer = Ref<AttachmentStubFramebuffer>::Create(391u, 491u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(393u, 493u);

    const auto bloomHandle = graph.ImportFramebuffer(ResourceNames::BloomColor,
                                                     bloomFramebuffer,
                                                     makeFramebufferDesc(ResourceNames::BloomColor, RGResourceFormat::RGBA16Float));
    const auto bloomTexture = graph.CreateFramebufferAttachmentView(ResourceNames::BloomColorTexture, bloomHandle, 0u);

    const auto dofHandle = graph.ImportFramebuffer(ResourceNames::DOFColor,
                                                   dofFramebuffer,
                                                   makeFramebufferDesc(ResourceNames::DOFColor, RGResourceFormat::RGBA16Float));
    const auto dofTexture = graph.CreateFramebufferAttachmentView(ResourceNames::DOFColorTexture, dofHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.BloomColor = bloomHandle;
    blackboard.Post.BloomColorTexture = bloomTexture;
    blackboard.Post.DOFColor = dofHandle;
    blackboard.Post.DOFColorTexture = dofTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto dofPass = Ref<DOFRenderPass>::Create();
    dofPass->SetName("DOFPass");
    dofPass->SetEnabled(true);

    auto motionBlurPass = Ref<MotionBlurRenderPass>::Create();
    motionBlurPass->SetName("MotionBlurPass");
    motionBlurPass->SetEnabled(false);

    auto taaPass = Ref<TAARenderPass>::Create();
    taaPass->SetName("TAAPass");
    taaPass->SetEnabled(false);

    auto precipitationPass = Ref<PrecipitationRenderPass>::Create();
    precipitationPass->SetName("PrecipitationPass");
    precipitationPass->SetEnabled(false);

    auto fogPass = Ref<FogRenderPass>::Create();
    fogPass->SetName("FogPass");
    fogPass->SetEnabled(false);

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(false);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, dofPass);
    AddPassNode(graph, motionBlurPass);
    AddPassNode(graph, taaPass);
    AddPassNode(graph, precipitationPass);
    AddPassNode(graph, fogPass);
    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = dofPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = dofPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "DOFColor@DOFPass");
    EXPECT_EQ(motionBlurPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(motionBlurPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(taaPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(taaPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(precipitationPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(precipitationPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(fogPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(fogPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(chromAbPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(chromAbPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::DOFColor), versionedOutput)
        << "Canonical DOFColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("DOFColor@DOFPass"), versionedOutput)
        << "Exact versioned DOFColor lookup should remain valid";
}

TEST(RenderGraph, MotionBlurPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto makeFramebufferDesc = [](std::string_view debugName, const RGResourceFormat colorFormat)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = 640u;
        desc.Height = 360u;
        desc.Attachments = { colorFormat, RGResourceFormat::R32Int, RGResourceFormat::RG16Float };
        desc.DebugName = std::string(debugName);
        return desc;
    };

    auto dofFramebuffer = Ref<AttachmentStubFramebuffer>::Create(391u, 491u);
    auto motionBlurFramebuffer = Ref<AttachmentStubFramebuffer>::Create(392u, 492u);
    auto uiFramebuffer = Ref<AttachmentStubFramebuffer>::Create(393u, 493u);

    const auto dofHandle = graph.ImportFramebuffer(ResourceNames::DOFColor,
                                                   dofFramebuffer,
                                                   makeFramebufferDesc(ResourceNames::DOFColor, RGResourceFormat::RGBA16Float));
    const auto dofTexture = graph.CreateFramebufferAttachmentView(ResourceNames::DOFColorTexture, dofHandle, 0u);

    const auto motionBlurHandle = graph.ImportFramebuffer(ResourceNames::MotionBlurColor,
                                                          motionBlurFramebuffer,
                                                          makeFramebufferDesc(ResourceNames::MotionBlurColor, RGResourceFormat::RGBA16Float));
    const auto motionBlurTexture = graph.CreateFramebufferAttachmentView(ResourceNames::MotionBlurColorTexture, motionBlurHandle, 0u);

    const auto uiHandle = graph.ImportFramebuffer(ResourceNames::UIComposite,
                                                  uiFramebuffer,
                                                  makeFramebufferDesc(ResourceNames::UIComposite, RGResourceFormat::RGBA8UNorm));
    const auto uiTexture = graph.CreateFramebufferAttachmentView(ResourceNames::UICompositeTexture, uiHandle, 0u);

    auto& blackboard = graph.GetBlackboard();
    blackboard.Post.DOFColor = dofHandle;
    blackboard.Post.DOFColorTexture = dofTexture;
    blackboard.Post.MotionBlurColor = motionBlurHandle;
    blackboard.Post.MotionBlurColorTexture = motionBlurTexture;
    blackboard.Post.UIComposite = uiHandle;
    blackboard.Post.UICompositeTexture = uiTexture;

    auto motionBlurPass = Ref<MotionBlurRenderPass>::Create();
    motionBlurPass->SetName("MotionBlurPass");
    motionBlurPass->SetEnabled(true);

    auto taaPass = Ref<TAARenderPass>::Create();
    taaPass->SetName("TAAPass");
    taaPass->SetEnabled(false);

    auto precipitationPass = Ref<PrecipitationRenderPass>::Create();
    precipitationPass->SetName("PrecipitationPass");
    precipitationPass->SetEnabled(false);

    auto fogPass = Ref<FogRenderPass>::Create();
    fogPass->SetName("FogPass");
    fogPass->SetEnabled(false);

    auto chromAbPass = Ref<ChromaticAberrationRenderPass>::Create();
    chromAbPass->SetName("ChromAberrationPass");
    chromAbPass->SetEnabled(false);

    auto colorGradingPass = Ref<ColorGradingRenderPass>::Create();
    colorGradingPass->SetName("ColorGradingPass");
    colorGradingPass->SetEnabled(false);

    auto toneMapPass = Ref<ToneMapRenderPass>::Create();
    toneMapPass->SetName("ToneMapPass");
    toneMapPass->SetEnabled(false);

    auto vignettePass = Ref<VignetteRenderPass>::Create();
    vignettePass->SetName("VignettePass");
    vignettePass->SetEnabled(false);

    auto uiCompositePass = Ref<UICompositeRenderPass>::Create();
    uiCompositePass->SetName("UICompositePass");

    AddPassNode(graph, motionBlurPass);
    AddPassNode(graph, taaPass);
    AddPassNode(graph, precipitationPass);
    AddPassNode(graph, fogPass);
    AddPassNode(graph, chromAbPass);
    AddPassNode(graph, colorGradingPass);
    AddPassNode(graph, toneMapPass);
    AddPassNode(graph, vignettePass);
    AddPassNode(graph, uiCompositePass);
    graph.SetFinalPass("UICompositePass");

    graph.BuildFrameGraph();

    const auto versionedOutput = motionBlurPass->GetPrimaryOutputFramebufferHandle();
    const auto versionedOutputTexture = motionBlurPass->GetPrimaryOutputTextureHandle();
    ASSERT_TRUE(versionedOutput.IsValid());
    ASSERT_TRUE(versionedOutputTexture.IsValid());
    EXPECT_EQ(graph.GetResourceName(versionedOutput), "MotionBlurColor@MotionBlurPass");
    EXPECT_EQ(taaPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(taaPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(precipitationPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(precipitationPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(fogPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(fogPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(chromAbPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(chromAbPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(colorGradingPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(toneMapPass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(toneMapPass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(vignettePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(vignettePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputFramebufferHandle(), versionedOutput);
    EXPECT_EQ(uiCompositePass->GetPrimaryInputTextureHandle(), versionedOutputTexture);
    EXPECT_EQ(graph.GetFramebufferHandle(ResourceNames::MotionBlurColor), versionedOutput)
        << "Canonical MotionBlurColor lookup should follow the latest explicit version";
    EXPECT_EQ(graph.GetFramebufferHandle("MotionBlurColor@MotionBlurPass"), versionedOutput)
        << "Exact versioned MotionBlurColor lookup should remain valid";
}

TEST(RenderGraph, AddNodeMakesItRetrievable)
{
    RenderGraph graph;

    auto node = Ref<TestGraphNode>::Create("GraphNodeA");
    graph.AddNode(node);

    auto retrieved = graph.GetNode<TestGraphNode>("GraphNodeA");

    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->GetName(), "GraphNodeA");
}

// Issue #530: ResetTopology() wipes the blackboard + imported-resource maps,
// but external per-frame caches that assume the blackboard survives between
// calls (RenderPipeline's blackboard-populate fingerprint) can't observe that
// wipe when every other hashed input is identical — which is exactly what
// happens when the same Deferred scene re-enters the render path twice in one
// process. GetTopologyGeneration() is their invalidation signal: it MUST
// advance on every topology teardown (ResetTopology / Shutdown) so a
// reconfigure still forces a repopulate. Without the bump, the second Deferred
// entry sees a matching fingerprint, skips repopulating the just-wiped
// blackboard, and every pass's Setup() reads empty handles -> the whole graph
// is culled (reads=0/writes=0).
TEST(RenderGraph, ResetTopologyAdvancesTopologyGenerationForCacheInvalidation)
{
    RenderGraph graph;

    const u64 gen0 = graph.GetTopologyGeneration();

    graph.ResetTopology();
    const u64 gen1 = graph.GetTopologyGeneration();
    EXPECT_GT(gen1, gen0) << "ResetTopology must advance the topology generation";

    // The bug shape is a reconfigure -> reconfigure sequence with no render in
    // between (TearDown path-restore then SetUp path-switch), so the generation
    // must keep advancing across back-to-back teardowns, never settling.
    graph.ResetTopology();
    const u64 gen2 = graph.GetTopologyGeneration();
    EXPECT_GT(gen2, gen1) << "every ResetTopology must advance the generation";

    graph.Shutdown();
    EXPECT_GT(graph.GetTopologyGeneration(), gen2)
        << "a full Shutdown also wipes the blackboard and must advance the generation";
}

TEST(RenderGraph, GraphNodeSetupAndExecuteUseCanonicalSubmissionPath)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto node = Ref<TestGraphNode>::Create(
        "GraphNodeExecute",
        [](RGBuilder& builder)
        {
            auto output = builder.ImportTexture(
                "GraphNodeOutput",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "GraphNodeOutput"));
            builder.Write(output, RGWriteUsage::RenderTarget);
        });

    graph.AddNode(node);
    graph.SetFinalPass("GraphNodeExecute");

    graph.BuildFrameGraph();
    graph.Execute();

    EXPECT_EQ(node->GetSetupCount(), 1u);
    EXPECT_EQ(node->GetExecuteCount(), 1u);
    EXPECT_TRUE(node->WasContextActive());
    EXPECT_EQ(node->GetObservedContextPassName(), "GraphNodeExecute");

    const auto plan = graph.GetSubmissionPlan();
    const auto nodeCommand = std::ranges::find_if(plan, [](const RenderGraph::SubmissionCommand& command)
                                                  { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::Pass &&
                                                           command.NodeName == "GraphNodeExecute"; });
    ASSERT_NE(nodeCommand, plan.end());
    EXPECT_NE(nodeCommand->NodePointer, nullptr);
}

TEST(RenderGraph, GraphNodesDeriveProducerConsumerDependencyFromDeclarations)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto producer = Ref<TestGraphNode>::Create(
        "GraphNodeProducer",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportTexture(
                "GraphNodeSharedColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "GraphNodeSharedColor"));
            builder.Write(shared, RGWriteUsage::RenderTarget);
        });

    auto consumer = Ref<TestGraphNode>::Create(
        "GraphNodeConsumer",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportTexture(
                "GraphNodeSharedColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "GraphNodeSharedColor"));
            [[maybe_unused]] const auto readShared = builder.Read(shared, RGReadUsage::ShaderSample);
        });

    graph.AddNode(producer);
    graph.AddNode(consumer);
    graph.SetFinalPass("GraphNodeConsumer");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto connections = graph.GetConnections();
    const auto derivedEdge = std::ranges::find_if(connections,
                                                  [](const RenderGraph::ConnectionInfo& connection)
                                                  {
                                                      return connection.OutputPass == "GraphNodeProducer" &&
                                                             connection.InputPass == "GraphNodeConsumer";
                                                  });
    EXPECT_NE(derivedEdge, connections.end());

    const auto& order = graph.GetExecutionOrder();
    const auto producerIt = std::ranges::find(order, "GraphNodeProducer");
    const auto consumerIt = std::ranges::find(order, "GraphNodeConsumer");
    ASSERT_NE(producerIt, order.end());
    ASSERT_NE(consumerIt, order.end());
    EXPECT_LT(std::distance(order.begin(), producerIt), std::distance(order.begin(), consumerIt));

    EXPECT_EQ(producer->GetExecuteCount(), 1u);
    EXPECT_EQ(consumer->GetExecuteCount(), 1u);
}

TEST(RenderGraph, GraphNodeReachabilityCullsUnusedNode)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto producer = Ref<TestGraphNode>::Create(
        "ReachableNodeProducer",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportTexture(
                "ReachableNodeColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ReachableNodeColor"));
            builder.Write(shared, RGWriteUsage::RenderTarget);
        });

    auto consumer = Ref<TestGraphNode>::Create(
        "ReachableNodeConsumer",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportTexture(
                "ReachableNodeColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ReachableNodeColor"));
            [[maybe_unused]] const auto readShared = builder.Read(shared, RGReadUsage::ShaderSample);
        });

    auto unused = Ref<TestGraphNode>::Create(
        "UnusedGraphNode",
        [](RGBuilder& builder)
        {
            auto unusedColor = builder.ImportTexture(
                "UnusedNodeColor",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "UnusedNodeColor"));
            builder.Write(unusedColor, RGWriteUsage::RenderTarget);
        });

    graph.AddNode(producer);
    graph.AddNode(consumer);
    graph.AddNode(unused);
    graph.SetFinalPass("ReachableNodeConsumer");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto& culled = graph.GetCulledPasses();
    EXPECT_NE(std::ranges::find(culled, "UnusedGraphNode"), culled.end());
    EXPECT_EQ(producer->GetExecuteCount(), 1u);
    EXPECT_EQ(consumer->GetExecuteCount(), 1u);
    EXPECT_EQ(unused->GetExecuteCount(), 0u);
}

TEST(RenderGraph, GraphNodeFlagsDriveSubmissionMetadata)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto node = Ref<TestGraphNode>::Create(
        "ComputeGraphNode",
        [](RGBuilder& builder)
        {
            auto output = builder.ImportBuffer(
                "ComputeGraphNodeOutput",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::StorageBuffer, "ComputeGraphNodeOutput"));
            builder.Write(output, RGWriteUsage::ShaderStorage);
        });
    node->SetFlags(RenderGraphNodeFlags::Compute |
                   RenderGraphNodeFlags::AsyncCandidateMetadata |
                   RenderGraphNodeFlags::UsesCommandBucket);

    graph.AddNode(node);
    graph.SetFinalPass("ComputeGraphNode");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto submissionInfo = graph.GetNodeSubmissionInfo();
    ASSERT_EQ(submissionInfo.size(), 1u);
    EXPECT_EQ(submissionInfo[0].NodeName, "ComputeGraphNode");
    EXPECT_TRUE(submissionInfo[0].DeclaresResources);
    EXPECT_EQ(submissionInfo[0].WorkType, RenderGraphPassWorkType::Compute);
    EXPECT_TRUE(submissionInfo[0].AsyncComputeCandidate);

    const auto plan = graph.GetSubmissionPlan();
    const auto nodeCommand = std::ranges::find_if(plan, [](const RenderGraph::SubmissionCommand& command)
                                                  { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::Pass &&
                                                           command.NodeName == "ComputeGraphNode"; });
    ASSERT_NE(nodeCommand, plan.end());
    EXPECT_EQ(nodeCommand->WorkType, RenderGraphPassWorkType::Compute);
    EXPECT_EQ(nodeCommand->Lane, RenderGraph::QueueLane::Compute);
    EXPECT_NE(nodeCommand->NodePointer, nullptr);

    const auto batchBegin = std::ranges::find_if(plan, [](const RenderGraph::SubmissionCommand& command)
                                                 { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::BatchBegin; });
    const auto batchEnd = std::ranges::find_if(plan, [](const RenderGraph::SubmissionCommand& command)
                                               { return command.CommandKind == RenderGraph::SubmissionCommand::Kind::BatchEnd; });
    EXPECT_NE(batchBegin, plan.end());
    EXPECT_NE(batchEnd, plan.end());
    EXPECT_EQ(node->GetExecuteCount(), 1u);
}

TEST(RenderGraph, GraphNodeLifecycleHooksTrackInitResizeAndRenderScale)
{
    RenderGraph graph;

    auto node = Ref<TestGraphNode>::Create("LifecycleGraphNode");
    graph.AddNode(node);

    graph.Init(640, 480);
    graph.Resize(320, 240);
    graph.SetRenderScale(0.5f);
    graph.SetRenderScale(1.0f);

    EXPECT_EQ(node->GetSetupFramebufferCount(), 1u);
    EXPECT_EQ(node->GetResizeFramebufferCount(), 1u);
    EXPECT_EQ(node->GetApplyRenderViewportCount(), 2u);
    EXPECT_EQ(node->GetLastSetupFramebufferWidth(), 640u);
    EXPECT_EQ(node->GetLastSetupFramebufferHeight(), 480u);
    EXPECT_EQ(node->GetLastResizeFramebufferWidth(), 320u);
    EXPECT_EQ(node->GetLastResizeFramebufferHeight(), 240u);
    EXPECT_EQ(node->GetLastRenderViewportWidth(), 0u);
    EXPECT_EQ(node->GetLastRenderViewportHeight(), 0u);
}

TEST(RenderGraph, PassAddedAfterGraphInitializationInheritsCurrentLifecycleState)
{
    RenderGraph graph;

    graph.Init(800, 600);
    graph.Resize(640, 480);
    graph.SetRenderScale(0.5f);

    auto pass = Ref<LifecycleTrackingPass>::Create("LateLifecyclePass");
    AddPassNode(graph, pass);

    EXPECT_EQ(pass->GetResizeFramebufferCount(), 1u);
    EXPECT_EQ(pass->GetApplyRenderViewportCount(), 1u);
    EXPECT_EQ(pass->GetLastResizeFramebufferWidth(), 640u);
    EXPECT_EQ(pass->GetLastResizeFramebufferHeight(), 480u);
    EXPECT_EQ(pass->GetLastRenderViewportWidth(), 320u);
    EXPECT_EQ(pass->GetLastRenderViewportHeight(), 240u);
}

TEST(RenderGraph, NodeAddedAfterGraphInitializationInheritsCurrentLifecycleState)
{
    RenderGraph graph;

    graph.Init(800, 600);
    graph.Resize(640, 480);
    graph.SetRenderScale(0.5f);

    auto node = Ref<TestGraphNode>::Create("LateLifecycleNode");
    graph.AddNode(node);

    EXPECT_EQ(node->GetSetupFramebufferCount(), 0u);
    EXPECT_EQ(node->GetResizeFramebufferCount(), 1u);
    EXPECT_EQ(node->GetApplyRenderViewportCount(), 1u);
    EXPECT_EQ(node->GetLastResizeFramebufferWidth(), 640u);
    EXPECT_EQ(node->GetLastResizeFramebufferHeight(), 480u);
    EXPECT_EQ(node->GetLastRenderViewportWidth(), 320u);
    EXPECT_EQ(node->GetLastRenderViewportHeight(), 240u);
}

TEST(RenderGraph, PlansBarrierFromWriterToReaderTransition)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            auto sharedBuffer = builder.ImportBuffer(
                "SharedBuffer",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            builder.Write(sharedBuffer, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "Consumer",
        [](RGBuilder& builder)
        {
            auto sharedBuffer = builder.ImportBuffer(
                "SharedBuffer",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            [[maybe_unused]] const auto readHandle = builder.Read(sharedBuffer, RGReadUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("Producer", "Consumer");
    graph.SetFinalPass("Consumer");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto& plannedBarriers = graph.GetPlannedBarriers();
    const auto barrierIt = std::ranges::find_if(plannedBarriers,
                                                [](const RenderGraph::PlannedBarrier& barrier)
                                                {
                                                    return barrier.BeforePass == "Consumer" &&
                                                           barrier.Resource == "SharedBuffer";
                                                });

    ASSERT_NE(barrierIt, plannedBarriers.end());
    const auto expectedFlags = MemoryBarrierFlags::ShaderStorage;
    EXPECT_EQ((barrierIt->Flags & expectedFlags), expectedFlags);
}

TEST(RenderGraph, PlansFramebufferAndTextureFetchBarrierForRenderTargetToSample)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "GBufferWrite",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColorTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColorTex"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "PostRead",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColorTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColorTex"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("GBufferWrite", "PostRead");
    graph.SetFinalPass("PostRead");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto& plannedBarriers = graph.GetPlannedBarriers();
    const auto barrierIt = std::ranges::find_if(plannedBarriers,
                                                [](const RenderGraph::PlannedBarrier& barrier)
                                                {
                                                    return barrier.BeforePass == "PostRead" &&
                                                           barrier.Resource == "SceneColorTex";
                                                });

    ASSERT_NE(barrierIt, plannedBarriers.end());
    const auto expected = MemoryBarrierFlags::Framebuffer | MemoryBarrierFlags::TextureFetch;
    EXPECT_EQ((barrierIt->Flags & expected), expected);
}

TEST(RenderGraph, ReportsMissingProducerDiagnosticForReadOnlyResource)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "ConsumerOnly",
        [](RGBuilder& builder)
        {
            auto orphanResource = builder.ImportTexture(
                "OrphanResource",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "OrphanResource"));
            [[maybe_unused]] const auto readHandle = builder.Read(orphanResource, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("ConsumerOnly");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::ranges::find_if(diagnostics,
                                             [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                             {
                                                 return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::MissingProducer &&
                                                        diagnostic.PassName == "ConsumerOnly" &&
                                                        diagnostic.Resource == "OrphanResource";
                                             });
    ASSERT_NE(diagIt, diagnostics.end());
}

TEST(RenderGraph, DerivedDependenciesAreRebuiltFromCurrentFrameDeclarations)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool optionalEnabled = true;

    auto scene = AddTestNode(
        graph,
        "Scene",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto optionalPost = AddTestNode(
        graph,
        "OptionalPost",
        [&optionalEnabled](RGBuilder& builder)
        {
            if (!optionalEnabled)
                return;

            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
            [[maybe_unused]] const auto readScene = builder.Read(sceneColor, RGReadUsage::ShaderSample);

            auto optionalColor = builder.ImportTexture(
                "OptionalColor",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "OptionalColor"));
            builder.Write(optionalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto final = AddTestNode(
        graph,
        "Final",
        [&optionalEnabled](RGBuilder& builder)
        {
            auto input = builder.ImportTexture(
                optionalEnabled ? "OptionalColor" : "SceneColor",
                optionalEnabled ? 2u : 1u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D,
                                               optionalEnabled ? "OptionalColor" : "SceneColor"));
            [[maybe_unused]] const auto readInput = builder.Read(input, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("Final");

    graph.BuildFrameGraph();
    graph.Execute();
    EXPECT_EQ(scene->GetExecuteCount(), 1u);
    EXPECT_EQ(optionalPost->GetExecuteCount(), 1u);
    EXPECT_EQ(final->GetExecuteCount(), 1u);

    optionalEnabled = false;
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_NE(std::ranges::find(culledPasses, "OptionalPost"), culledPasses.end())
        << "Derived edges from the enabled frame must not keep a disabled pass reachable";

    const auto connections = graph.GetConnections();
    const auto staleEdge = std::ranges::find_if(connections,
                                                [](const RenderGraph::ConnectionInfo& connection)
                                                {
                                                    return connection.OutputPass == "OptionalPost" ||
                                                           connection.InputPass == "OptionalPost";
                                                });
    EXPECT_EQ(staleEdge, connections.end()) << "Disabled pass should not keep stale derived dependencies";

    EXPECT_EQ(scene->GetExecuteCount(), 2u);
    EXPECT_EQ(optionalPost->GetExecuteCount(), 1u);
    EXPECT_EQ(final->GetExecuteCount(), 2u);
}

TEST(RenderGraph, ReportsStaleExtractionHandleDiagnostic)
{
    RenderGraph graph;

    const auto stale = graph.ImportTexture(
        "TemporalColor",
        7,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "TemporalColor"));

    graph.ClearImportedResources();

    bool extractionCalled = false;
    graph.ExtractTexture(stale,
                         [&extractionCalled](u32 /*id*/)
                         {
                             extractionCalled = true;
                         });

    graph.Execute();

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::ranges::find_if(diagnostics,
                                             [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                             {
                                                 return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::StaleExtractionHandle;
                                             });

    ASSERT_NE(diagIt, diagnostics.end());
    EXPECT_FALSE(extractionCalled);
}

TEST(RenderGraph, ReportsExtractionOfCulledResourceDiagnostic)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "CulledProducer",
        [](RGBuilder& builder)
        {
            auto transientColor = builder.ImportTexture(
                "CulledColor",
                11,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CulledColor"));
            builder.Write(transientColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                12,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto culledHandle = graph.GetTextureHandle("CulledColor");
    ASSERT_TRUE(culledHandle.IsValid());

    bool extractionCalled = false;
    graph.ExtractTexture(culledHandle,
                         [&extractionCalled](u32 /*id*/)
                         {
                             extractionCalled = true;
                         });

    graph.Execute();

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::ranges::find_if(diagnostics,
                                             [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                             {
                                                 return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::ExtractionOfCulledResource &&
                                                        diagnostic.Resource == "CulledColor";
                                             });

    ASSERT_NE(diagIt, diagnostics.end());
    EXPECT_FALSE(extractionCalled);
}

TEST(RenderGraph, ExtractTextureBeforeBuildRootsProducerAndInvokesCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto extractedColor = graph.ImportTexture(
        "ExtractedColor",
        11u,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ExtractedColor"));

    AddTestNode(
        graph,
        "ExtractOnlyProducer",
        [extractedColor](RGBuilder& builder)
        {
            builder.Write(extractedColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                12u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    bool extractionCalled = false;
    u32 extractedID = 0u;
    graph.ExtractTexture(extractedColor,
                         [&extractionCalled, &extractedID](u32 textureID)
                         {
                             extractionCalled = true;
                             extractedID = textureID;
                         });

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_EQ(std::ranges::find(culledPasses, "ExtractOnlyProducer"), culledPasses.end());

    graph.Execute();

    EXPECT_TRUE(extractionCalled);
    EXPECT_EQ(extractedID, 11u);

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::ranges::find_if(diagnostics,
                                             [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                             {
                                                 return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::ExtractionOfCulledResource &&
                                                        diagnostic.Resource == "ExtractedColor";
                                             });
    EXPECT_EQ(diagIt, diagnostics.end());
}

TEST(RenderGraph, ExtractFramebufferBeforeBuildRootsProducerAndInvokesCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc scratchDesc;
    scratchDesc.Kind = ResourceHandle::Kind::Framebuffer;
    scratchDesc.Format = RGResourceFormat::RGBA16Float;
    scratchDesc.Width = 640u;
    scratchDesc.Height = 360u;

    const auto extractedFramebuffer = graph.DeclareTransientFramebuffer("ExtractedFramebuffer", scratchDesc);

    AddTestNode(
        graph,
        "ExtractOnlyProducer",
        [extractedFramebuffer](RGBuilder& builder)
        {
            builder.Write(extractedFramebuffer, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                12u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    bool extractionCalled = false;
    graph.ExtractFramebuffer(extractedFramebuffer,
                             [&extractionCalled](const Ref<Framebuffer>& /*framebuffer*/)
                             {
                                 extractionCalled = true;
                             });

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_EQ(std::ranges::find(culledPasses, "ExtractOnlyProducer"), culledPasses.end());

    graph.Execute();

    EXPECT_TRUE(extractionCalled);

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::ranges::find_if(diagnostics,
                                             [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                             {
                                                 return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::ExtractionOfCulledResource &&
                                                        diagnostic.Resource == "ExtractedFramebuffer";
                                             });
    EXPECT_EQ(diagIt, diagnostics.end());
}

TEST(RenderGraph, DumpToJsonWritesCompiledGraphDetails)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportBuffer(
                "SharedBuffer",
                41,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            builder.Write(shared, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportBuffer(
                "SharedBuffer",
                41,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            [[maybe_unused]] const auto readHandle = builder.Read(shared, RGReadUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_phase_e_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());

    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"timingVersion\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"hasTimings\": true"), std::string::npos);
    EXPECT_NE(json.find("\"frameSummary\""), std::string::npos);
    EXPECT_NE(json.find("\"passCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"resourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"culledPassCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"timingsCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"computePassCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"asyncComputeCandidateCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"historyResourceCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"externallyBackedTransientRootCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"externallyBackedResourceCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"temporalHistoryContractCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"passFlags\""), std::string::npos);
    EXPECT_NE(json.find("\"workType\": \"Graphics\""), std::string::npos);
    EXPECT_NE(json.find("\"asyncComputeCandidate\": false"), std::string::npos);
    EXPECT_NE(json.find("\"buildStats\""), std::string::npos);
    EXPECT_NE(json.find("\"passesVisited\":"), std::string::npos);
    EXPECT_NE(json.find("\"declaredReads\":"), std::string::npos);
    EXPECT_NE(json.find("\"declaredWrites\":"), std::string::npos);
    EXPECT_NE(json.find("\"derivedEdges\":"), std::string::npos);
    EXPECT_NE(json.find("\"orderSensitiveResults\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"buildDiagnosticCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"buildDiagnostics\""), std::string::npos);
    EXPECT_NE(json.find("\"passOrder\""), std::string::npos);
    EXPECT_NE(json.find("\"passAuthoring\""), std::string::npos);
    EXPECT_NE(json.find("\"plannedBarriers\""), std::string::npos);
    EXPECT_NE(json.find("\"barrierDiagnostics\""), std::string::npos);
    EXPECT_NE(json.find("\"lifetimes\""), std::string::npos);
    EXPECT_NE(json.find("\"accessModes\""), std::string::npos);
    EXPECT_NE(json.find("\"resources\""), std::string::npos);
    EXPECT_NE(json.find("\"isHistory\": false"), std::string::npos);
    EXPECT_NE(json.find("\"textureID\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"bufferID\": 41"), std::string::npos);
    EXPECT_NE(json.find("\"framebufferID\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"framebufferColor0ID\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"temporalHistoryContracts\""), std::string::npos);
    EXPECT_NE(json.find("\"timingSummary\""), std::string::npos);
    EXPECT_NE(json.find("\"executedPasses\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"culledPasses\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"totalCpuMs\":"), std::string::npos);
    EXPECT_NE(json.find("\"averageCpuMs\":"), std::string::npos);
    EXPECT_NE(json.find("\"maxCpuMs\":"), std::string::npos);
    EXPECT_NE(json.find("\"maxPass\":"), std::string::npos);
    EXPECT_NE(json.find("\"executionTimeline\""), std::string::npos);
    EXPECT_NE(json.find("\"orderIndex\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"executed\": true"), std::string::npos);
    EXPECT_NE(json.find("\"culled\": false"), std::string::npos);
    EXPECT_NE(json.find("\"timingStatsByPass\""), std::string::npos);
    EXPECT_NE(json.find("\"Producer\": { \"orderIndex\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"Final\": { \"orderIndex\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"timingDigest\""), std::string::npos);
    EXPECT_NE(json.find("\"unit\": \"cpuUs\""), std::string::npos);
    EXPECT_NE(json.find("\"entryCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"concat\":"), std::string::npos);
    EXPECT_NE(json.find("Producer@0="), std::string::npos);
    EXPECT_NE(json.find("Final@1="), std::string::npos);
    EXPECT_NE(json.find("\"resourceDigest\": { \"version\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"resourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"lifetimeCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"accessCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"aliasCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("res:SharedBuffer:StorageBuffer:1:xb0"), std::string::npos);
    EXPECT_NE(json.find("life:SharedBuffer@0-1:r1:w1"), std::string::npos);
    EXPECT_NE(json.find("acc:SharedBuffer:r[ShaderStorage]:w[ShaderStorage]"), std::string::npos);
    EXPECT_NE(json.find("\"barrierDigest\": { \"version\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"plannedCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"diagnosticCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"missingProducerCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"entryCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("bar:Final/SharedBuffer/"), std::string::npos);
    EXPECT_NE(json.find("\"graphDigest\": { \"version\": 1"), std::string::npos);
    EXPECT_NE(json.find("passes=2;resources=1;culled=0;barriers=1;diags=0;aliases=0;timings=2;compute=0;asyncCandidates=0"), std::string::npos);
    EXPECT_NE(json.find("histories=0"), std::string::npos);
    EXPECT_NE(json.find("externalTextureSinks=0"), std::string::npos);
    EXPECT_NE(json.find("historyContracts=0"), std::string::npos);
    EXPECT_NE(json.find("\"timings\""), std::string::npos);
    EXPECT_NE(json.find("\"orderIndex\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"cpuMs\":"), std::string::npos);
    EXPECT_NE(json.find("\"SharedBuffer\""), std::string::npos);
    EXPECT_NE(json.find("\"ShaderStorage\""), std::string::npos);
    EXPECT_NE(json.find("\"Producer\""), std::string::npos);
    EXPECT_NE(json.find("\"Final\""), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraph, BuilderExtractTextureRootsProducerAndInvokesCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool extractionCalled = false;
    u32 extractedID = 0u;

    AddTestNode(
        graph,
        "BuilderExtractProducer",
        [&extractionCalled, &extractedID](RGBuilder& builder)
        {
            const auto extractedColor = builder.ImportTexture(
                "BuilderExtractColor",
                21u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "BuilderExtractColor"));
            builder.Write(extractedColor, RGWriteUsage::RenderTarget);
            builder.ExtractTexture(
                extractedColor,
                [&extractionCalled, &extractedID](const u32 textureID)
                {
                    extractionCalled = true;
                    extractedID = textureID;
                });
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                22u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_EQ(std::ranges::find(culledPasses, "BuilderExtractProducer"), culledPasses.end());

    graph.Execute();

    EXPECT_TRUE(extractionCalled);
    EXPECT_EQ(extractedID, 21u);
}

TEST(RenderGraph, BuilderExtractFramebufferRootsProducerAndInvokesCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool extractionCalled = false;

    AddTestNode(
        graph,
        "BuilderExtractProducer",
        [&extractionCalled](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 320u;
            desc.Height = 180u;

            const auto extractedFramebuffer = builder.CreateFramebuffer("BuilderExtractFramebuffer", desc);
            builder.Write(extractedFramebuffer, RGWriteUsage::RenderTarget);
            builder.ExtractFramebuffer(
                extractedFramebuffer,
                [&extractionCalled](const Ref<Framebuffer>& /*framebuffer*/)
                {
                    extractionCalled = true;
                });
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                22u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_EQ(std::ranges::find(culledPasses, "BuilderExtractProducer"), culledPasses.end());

    graph.Execute();

    EXPECT_TRUE(extractionCalled);
}

TEST(RenderGraphExternalTextureSinks, BuilderRegisteredTextureSinkRootsProducerAndInvalidatesWithoutNewCopy)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool sinkValid = true;

    AddTestNode(
        graph,
        "ExternalSinkProducer",
        [&sinkValid](RGBuilder& builder)
        {
            const auto source = builder.ImportTexture(
                "ExternalSinkColor",
                31u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ExternalSinkColor"));
            builder.Write(source, RGWriteUsage::RenderTarget);
            builder.RegisterExternalTextureSink(source, 0u, 0u, 0u, &sinkValid);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                32u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_EQ(std::ranges::find(culledPasses, "ExternalSinkProducer"), culledPasses.end());

    const auto& contracts = graph.GetExternalTextureSinkContracts();
    ASSERT_EQ(contracts.size(), 1u);
    EXPECT_EQ(contracts[0].SourceResource, "ExternalSinkColor");
    EXPECT_EQ(contracts[0].SourceKind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(contracts[0].ColorAttachmentIndex, 0u);
    EXPECT_TRUE(contracts[0].SourceReachable);

    graph.Execute();

    EXPECT_FALSE(sinkValid);
}

TEST(RenderGraphExternalTextureSinks, BuilderRegisteredFramebufferSinkRootsProducerAndKeepsAttachmentIndex)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool sinkValid = true;

    AddTestNode(
        graph,
        "ExternalSinkProducer",
        [&sinkValid](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 320u;
            desc.Height = 180u;

            const auto source = builder.CreateFramebuffer("ExternalSinkFramebuffer", desc);
            builder.Write(source, RGWriteUsage::RenderTarget);
            builder.RegisterExternalTextureSink(source, 0u, 0u, 0u, 0u, &sinkValid);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                32u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_EQ(std::ranges::find(culledPasses, "ExternalSinkProducer"), culledPasses.end());

    const auto& contracts = graph.GetExternalTextureSinkContracts();
    ASSERT_EQ(contracts.size(), 1u);
    EXPECT_EQ(contracts[0].SourceResource, "ExternalSinkFramebuffer");
    EXPECT_EQ(contracts[0].SourceKind, ResourceHandle::Kind::Framebuffer);
    EXPECT_EQ(contracts[0].ColorAttachmentIndex, 0u);
    EXPECT_TRUE(contracts[0].SourceReachable);

    graph.Execute();

    EXPECT_FALSE(sinkValid);
}

TEST(RenderGraphExternalTextureSinks, DumpToJsonIncludesExternalTextureSinkContracts)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool sinkValid = false;

    AddTestNode(
        graph,
        "ExternalSinkProducer",
        [&sinkValid](RGBuilder& builder)
        {
            const auto source = builder.ImportTexture(
                "ExternalSinkColor",
                41u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ExternalSinkColor"));
            builder.Write(source, RGWriteUsage::RenderTarget);
            builder.RegisterExternalTextureSink(source, 0u, 0u, 0u, &sinkValid);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            const auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                42u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_external_sink_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"externalTextureSinkContractCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"externalTextureSinkContracts\""), std::string::npos);
    EXPECT_NE(json.find("\"sourceResource\": \"ExternalSinkColor\""), std::string::npos);
    EXPECT_NE(json.find("\"sourceKind\": \"Texture2D\""), std::string::npos);
    EXPECT_NE(json.find("\"sourceReachable\": true"), std::string::npos);
    EXPECT_NE(json.find("externalTextureSinks=1"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// =============================================================================
// Topological Order Respects DAG
// =============================================================================

TEST(RenderGraph, LinearChainOrder)
{
    // A -> B -> C
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");

    // Execute triggers UpdateDependencyGraph
    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 3u);

    // A must appear before B, and B before C
    auto posA = std::ranges::find(order, "A") - order.begin();
    auto posB = std::ranges::find(order, "B") - order.begin();
    auto posC = std::ranges::find(order, "C") - order.begin();

    EXPECT_LT(posA, posB) << "A must execute before B";
    EXPECT_LT(posB, posC) << "B must execute before C";
}

TEST(RenderGraph, DiamondDependency)
{
    //   A
    //  / \.
    // B   C
    //  \ /
    //   D
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");
    AddStub(graph, "D");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "C");
    graph.ConnectPass("B", "D");
    graph.ConnectPass("C", "D");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 4u);

    auto posA = std::ranges::find(order, "A") - order.begin();
    auto posB = std::ranges::find(order, "B") - order.begin();
    auto posC = std::ranges::find(order, "C") - order.begin();
    auto posD = std::ranges::find(order, "D") - order.begin();

    EXPECT_LT(posA, posB);
    EXPECT_LT(posA, posC);
    EXPECT_LT(posB, posD);
    EXPECT_LT(posC, posD);
}

// =============================================================================
// Execution Dependency (ordering without framebuffer piping)
// =============================================================================

TEST(RenderGraph, ExecutionDependencyOrdering)
{
    RenderGraph graph;
    AddStub(graph, "Shadow");
    AddStub(graph, "Scene");

    // Shadow must run before Scene (e.g., shadow map texture dependency)
    graph.AddExecutionDependency("Shadow", "Scene");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 2u);

    auto posShadow = std::ranges::find(order, "Shadow") - order.begin();
    auto posScene = std::ranges::find(order, "Scene") - order.begin();

    EXPECT_LT(posShadow, posScene) << "Shadow must execute before Scene";
}

// =============================================================================
// All Passes Present in Order
// =============================================================================

TEST(RenderGraph, AllPassesPresentInOrder)
{
    RenderGraph graph;
    std::vector<std::string> names = { "GBuffer", "Lighting", "PostProcess", "UI", "Final" };

    for (const auto& n : names)
    {
        AddStub(graph, n);
    }

    // Chain: GBuffer -> Lighting -> PostProcess -> Final
    graph.ConnectPass("GBuffer", "Lighting");
    graph.ConnectPass("Lighting", "PostProcess");
    graph.ConnectPass("PostProcess", "Final");
    // UI is independent
    graph.AddExecutionDependency("PostProcess", "UI");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    EXPECT_EQ(order.size(), names.size());

    // Verify all names present
    std::unordered_set<std::string> orderSet(order.begin(), order.end());
    for (const auto& n : names)
    {
        EXPECT_TRUE(orderSet.count(n) > 0) << "Pass '" << n << "' missing from execution order";
    }
}

// =============================================================================
// Independent Passes (no dependencies)
// =============================================================================

TEST(RenderGraph, IndependentPassesAllExecute)
{
    RenderGraph graph;
    auto a = AddStub(graph, "IndependentA");
    auto b = AddStub(graph, "IndependentB");
    auto c = AddStub(graph, "IndependentC");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    EXPECT_EQ(order.size(), 3u);

    // Graphs without an explicit final pass keep all independent passes
    // reachable, so every registered pass still executes.
    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 1u);
}

// =============================================================================
// Final Pass
// =============================================================================

TEST(RenderGraph, SetFinalPassIsFinalPass)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");

    graph.SetFinalPass("B");

    EXPECT_TRUE(graph.IsFinalPass("B"));
    EXPECT_FALSE(graph.IsFinalPass("A"));
}

TEST(RenderGraph, UnreachablePassesAreCulledFromExecution)
{
    RenderGraph graph;
    auto a = AddStub(graph, "A");
    auto b = AddStub(graph, "B");
    auto c = AddStub(graph, "C");

    // Reachable chain: A -> B (final). C is isolated and should be culled.
    graph.ConnectPass("A", "B");
    graph.SetFinalPass("B");

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 0u) << "Unreachable pass should be culled";
}

TEST(RenderGraph, SideEffectingUnreachablePassStillExecutes)
{
    RenderGraph graph;
    auto a = AddStub(graph, "A");
    auto b = AddStub(graph, "B");
    auto c = AddStub(graph, "ReadbackPass");

    // Reachable chain: A -> B (final).
    graph.ConnectPass("A", "B");
    graph.SetFinalPass("B");

    // Mark isolated pass as side-effecting (e.g. GPU readback).
    c->SetSideEffects(RenderGraphNode::SideEffect::Readback);

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 1u) << "Side-effecting pass must not be culled";
}

// =============================================================================
// Layer 5 structural validation — production ordering invariants
//
// These tests encode the engine's pipeline contract: regardless of how passes
// are added or connected, a correctly-authored RenderGraph must topologically
// sort them so shadow rendering precedes scene rendering, scene precedes
// post-processing, and post-processing precedes the final screen blit.
//
// Failures here indicate the render graph violates its fundamental ordering
// contract — a class of bugs that would produce missing shadows, ungraded
// output, or tone-mapped geometry being post-processed twice.
// =============================================================================

TEST(RenderGraphStructural, ProductionPassOrderingAlwaysRespected)
{
    RenderGraph graph;
    // Deliberately add in a random order to prove topological sort doesn't
    // rely on insertion order.
    AddStub(graph, "FinalPass");
    AddStub(graph, "ScenePass");
    AddStub(graph, "ShadowPass");
    AddStub(graph, "PostProcessPass");

    graph.AddExecutionDependency("ShadowPass", "ScenePass");
    graph.ConnectPass("ScenePass", "PostProcessPass");
    graph.ConnectPass("PostProcessPass", "FinalPass");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    auto posOf = [&](const std::string& n)
    {
        return std::ranges::find(order, n) - order.begin();
    };
    const auto shadowPos = posOf("ShadowPass");
    const auto scenePos = posOf("ScenePass");
    const auto postPos = posOf("PostProcessPass");
    const auto finalPos = posOf("FinalPass");

    // std::find returns end() for missing names, whose index equals order.size().
    // Without these existence asserts the EXPECT_LT comparisons below could
    // silently pass (both sides clamped to size()) when the pass dropped out
    // of the execution order entirely.
    const auto orderSize = static_cast<decltype(shadowPos)>(order.size());
    ASSERT_LT(shadowPos, orderSize) << "ShadowPass missing from execution order";
    ASSERT_LT(scenePos, orderSize) << "ScenePass missing from execution order";
    ASSERT_LT(postPos, orderSize) << "PostProcessPass missing from execution order";
    ASSERT_LT(finalPos, orderSize) << "FinalPass missing from execution order";

    EXPECT_LT(shadowPos, scenePos) << "Shadow must precede Scene";
    EXPECT_LT(scenePos, postPos) << "Scene must precede PostProcess";
    EXPECT_LT(postPos, finalPos) << "PostProcess must precede Final";
}

TEST(RenderGraphStructural, FinalPresentSinkUsesOrderingOnly)
{
    RenderGraph graph;
    auto post = Ref<InputTrackingStubPass>::Create("PostProcessPass");
    auto ui = Ref<InputTrackingStubPass>::Create("UICompositePass");
    auto final = Ref<InputTrackingStubPass>::Create("FinalPass");
    (void)ui;
    AddPassNode(graph, post);
    AddPassNode(graph, ui);
    AddPassNode(graph, final);

    // Phase F contract: FinalPass is a side-effecting present sink.
    // UIComposite -> Final must be ordering-only, not framebuffer piping.
    graph.ConnectPass("PostProcessPass", "UICompositePass");
    graph.AddExecutionDependency("UICompositePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    graph.Execute();

    EXPECT_EQ(final->GetExecuteCount(), 1u)
        << "FinalPass must still execute via ordering dependency";

    const auto& order = graph.GetExecutionOrder();
    auto posOf = [&](const std::string& n)
    {
        return std::ranges::find(order, n) - order.begin();
    };
    const auto uiPos = posOf("UICompositePass");
    const auto finalPos = posOf("FinalPass");
    const auto orderSize = static_cast<decltype(uiPos)>(order.size());
    ASSERT_LT(uiPos, orderSize) << "UICompositePass missing from execution order";
    ASSERT_LT(finalPos, orderSize) << "FinalPass missing from execution order";
    EXPECT_LT(uiPos, finalPos) << "Ordering dependency must place UIComposite before Final";
}

TEST(RenderGraphStructural, DuplicateConnectPassIsIdempotent)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "B"); // Duplicate: must not be added twice
    graph.ConnectPass("A", "B");

    const auto connections = graph.GetConnections();
    u32 abCount = 0;
    for (const auto& c : connections)
    {
        if (c.OutputPass == "A" && c.InputPass == "B")
            ++abCount;
    }
    EXPECT_EQ(abCount, 1u) << "Duplicate ConnectPass calls must not register multiple edges";
}

TEST(RenderGraphStructural, EachPassExecutesExactlyOncePerExecute)
{
    RenderGraph graph;
    auto a = AddStub(graph, "A");
    auto b = AddStub(graph, "B");
    auto c = AddStub(graph, "C");

    // Diamond — B and C both depend on A, and the engine previously had bugs
    // where a pass with multiple downstream consumers ran more than once.
    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "C");

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u) << "A with 2 downstreams must still run once";
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 1u);

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 2u);
    EXPECT_EQ(b->GetExecuteCount(), 2u);
    EXPECT_EQ(c->GetExecuteCount(), 2u);
}

TEST(RenderGraphStructural, CycleIsDetectedAndDoesNotCrash)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");

    // Add a cycle A → B → A. Topological sort must NOT infinite-loop.
    // We tolerate either outcome: cycle rejected silently or passes
    // dropped. The binding contract is "no infinite loop / no crash."
    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "A");

    EXPECT_NO_THROW(graph.Execute())
        << "Cycle in render graph must not crash execution";
}

TEST(RenderGraphStructural, DerivedEdgeDoesNotIntroduceReverseCycle)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    // BuildFrameGraph will see both passes write SceneColor. Without a
    // cycle guard, insertion order (Water before Decal) can derive the
    // reverse Water -> Decal edge and create a cycle.
    AddTestNode(
        graph,
        "WaterPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "DecalPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    // Explicit production ordering contract.
    graph.AddExecutionDependency("DecalPass", "WaterPass");

    graph.SetFinalPass("WaterPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const auto hasCycle = std::ranges::any_of(hazards,
                                              [](const RenderGraph::Hazard& h)
                                              {
                                                  return h.Kind == RenderGraph::HazardKind::Cycle;
                                              });
    EXPECT_FALSE(hasCycle) << "Derived edge insertion must not create cycles";
    EXPECT_EQ(graph.GetExecutionOrder().size(), 2u)
        << "Cycle guard must preserve a valid two-pass topological order";
}

TEST(RenderGraphStructural, BuilderPassDependenciesOverrideReverseRegistrationOrder)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto addSceneColorWriter = [&graph](const char* passName, const char* dependency)
    {
        AddSetupNode(
            graph,
            passName,
            [dependency](RGBuilder& builder)
            {
                builder.DependsOnPass(dependency);

                auto sceneColor = builder.ImportTexture(
                    "SceneColor",
                    1u,
                    RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
                builder.Write(sceneColor, RGWriteUsage::RenderTarget);
            });
    };

    // Intentionally register the chain in reverse order. Builder-declared
    // pass dependencies must still enforce the documented production order.
    addSceneColorWriter("WaterPass", "DecalPass");
    addSceneColorWriter("DecalPass", "FoliagePass");
    addSceneColorWriter("FoliagePass", "ForwardOverlayPass");
    addSceneColorWriter("ForwardOverlayPass", "DeferredLightingPass");
    addSceneColorWriter("DeferredLightingPass", "DeferredOpaqueDecalPass");
    addSceneColorWriter("DeferredOpaqueDecalPass", "ScenePass");
    addSceneColorWriter("ScenePass", "ShadowPass");

    AddSetupNode(
        graph,
        "ShadowPass",
        [](RGBuilder& builder)
        {
            auto shadowMap = builder.ImportTexture(
                "ShadowMapCSM",
                2u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ShadowMapCSM"));
            builder.Write(shadowMap, RGWriteUsage::DepthStencil);
        });

    graph.SetFinalPass("WaterPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const auto hasCycle = std::ranges::any_of(hazards,
                                              [](const RenderGraph::Hazard& h)
                                              {
                                                  return h.Kind == RenderGraph::HazardKind::Cycle;
                                              });
    EXPECT_FALSE(hasCycle) << "Builder pass dependencies must stay cycle-free";

    const auto& order = graph.GetExecutionOrder();
    auto posOf = [&order](const char* name)
    {
        return std::ranges::find(order, name) - order.begin();
    };

    EXPECT_LT(posOf("ShadowPass"), posOf("ScenePass"));
    EXPECT_LT(posOf("ScenePass"), posOf("DeferredOpaqueDecalPass"));
    EXPECT_LT(posOf("DeferredOpaqueDecalPass"), posOf("DeferredLightingPass"));
    EXPECT_LT(posOf("DeferredLightingPass"), posOf("ForwardOverlayPass"));
    EXPECT_LT(posOf("ForwardOverlayPass"), posOf("FoliagePass"));
    EXPECT_LT(posOf("FoliagePass"), posOf("DecalPass"));
    EXPECT_LT(posOf("DecalPass"), posOf("WaterPass"));
}

TEST(RenderGraphStructural, DerivedEdgesSatisfyDeferredCoreWithoutManualEdges)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));

            builder.Write(sceneDepth, RGWriteUsage::DepthStencil);
            builder.Write(sceneNormals, RGWriteUsage::RenderTarget);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "DeferredOpaqueDecalPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "DeferredLightingPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalsRead = builder.Read(sceneNormals, RGReadUsage::ShaderSample);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));
            [[maybe_unused]] const auto sceneColorRead = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "Derived deferred-core edges should satisfy hazards";

    const auto& order = graph.GetExecutionOrder();
    auto posOf = [&](const char* name)
    {
        return std::ranges::find(order, name) - order.begin();
    };
    EXPECT_LT(posOf("ScenePass"), posOf("DeferredOpaqueDecalPass"));
    EXPECT_LT(posOf("DeferredOpaqueDecalPass"), posOf("DeferredLightingPass"));
    EXPECT_LT(posOf("DeferredLightingPass"), posOf("FinalPass"));
}

TEST(RenderGraphStructural, DerivedEdgesSatisfySceneToSSAOWithoutManualEdge)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            builder.Write(sceneDepth, RGWriteUsage::DepthStencil);
            builder.Write(sceneNormals, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "SSAOPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalsRead = builder.Read(sceneNormals, RGReadUsage::ShaderSample);
            builder.Write(ao, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));
            [[maybe_unused]] const auto aoRead = builder.Read(ao, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "Derived Scene->SSAO edge should satisfy hazards";
}

TEST(RenderGraphStructural, DerivedEdgesSatisfySceneToGTAOWithoutManualEdge)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddTestNode(
        graph,
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            builder.Write(sceneDepth, RGWriteUsage::DepthStencil);
            builder.Write(sceneNormals, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "GTAOPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalsRead = builder.Read(sceneNormals, RGReadUsage::ShaderSample);
            builder.Write(ao, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

    AddTestNode(
        graph,
        "FinalPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));
            [[maybe_unused]] const auto aoRead = builder.Read(ao, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "Derived Scene->GTAO edge should satisfy hazards";
}

TEST(RenderGraphStructural, ConnectingToMissingPassDoesNotCorruptGraph)
{
    RenderGraph graph;
    AddStub(graph, "A");

    // Neither input nor output exists — engine logs an error but graph
    // state must remain consistent.
    graph.ConnectPass("A", "Nonexistent");
    graph.ConnectPass("Nonexistent", "A");

    EXPECT_NO_THROW(graph.Execute());
    EXPECT_EQ(graph.GetNodeSubmissionInfo().size(), 1u)
        << "Connect calls with missing graph entries must not register new entries";
}

// =============================================================================
// GetConnections Returns All Connections
// =============================================================================

TEST(RenderGraph, GetConnectionsComplete)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");

    auto connections = graph.GetConnections();
    EXPECT_GE(connections.size(), 2u);
}

// =============================================================================
// Multiple Execute Calls Are Idempotent
// =============================================================================

TEST(RenderGraph, MultipleExecuteIdempotent)
{
    RenderGraph graph;
    auto pass = AddStub(graph, "Repeater");

    graph.Execute();
    graph.Execute();
    graph.Execute();

    EXPECT_EQ(pass->GetExecuteCount(), 3u);

    const auto& order = graph.GetExecutionOrder();
    EXPECT_EQ(order.size(), 1u);
}

// =============================================================================
// Single Pass Graph
// =============================================================================

TEST(RenderGraph, SinglePassGraph)
{
    RenderGraph graph;
    auto pass = AddStub(graph, "Only");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "Only");
    EXPECT_EQ(pass->GetExecuteCount(), 1u);
}

// =============================================================================
// ResetTopology: per-RenderingPath rebuild contract (Option 3)
//
// Renderer3D::ConfigureRenderGraph calls RenderGraph::ResetTopology() and
// then re-registers only the passes relevant for the active RenderingPath.
// These tests pin down the ResetTopology primitive that the rebuild
// depends on.
// =============================================================================

TEST(RenderGraphResetTopology, ClearsPassesAndAllowsRebuild)
{
    RenderGraph graph;

    // Initial "deferred-like" topology: 3 passes, a chain.
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");
    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");
    graph.SetFinalPass("C");
    graph.Execute();
    EXPECT_EQ(graph.GetNodeSubmissionInfo().size(), 3u);
    EXPECT_EQ(graph.GetExecutionOrder().size(), 3u);

    graph.ResetTopology();

    // After reset the graph must behave as freshly constructed: no
    // passes, no cached order, no stale connections leaking into the
    // next rebuild.
    EXPECT_EQ(graph.GetNodeSubmissionInfo().size(), 0u);
    EXPECT_EQ(graph.GetConnections().size(), 0u);

    // Rebuild as a different "forward-like" topology (2 passes).
    AddStub(graph, "X");
    AddStub(graph, "Y");
    graph.ConnectPass("X", "Y");
    graph.SetFinalPass("Y");
    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "X");
    EXPECT_EQ(order[1], "Y");
    EXPECT_EQ(graph.GetNode<RenderGraphNode>("A"), nullptr)
        << "Old graph entries must not be retrievable after ResetTopology";
    EXPECT_EQ(graph.GetNode<RenderGraphNode>("B"), nullptr);
    EXPECT_EQ(graph.GetNode<RenderGraphNode>("C"), nullptr);
}

TEST(RenderGraphResetTopology, PreservesPassReferenceOwnership)
{
    // ResetTopology must only drop the graph's references, not destroy
    // passes still held by external owners (in the real engine that's
    // Renderer3D::s_Data). The test below verifies that external Refs keep
    // the pass alive after the graph forgets it.
    RenderGraph graph;
    auto a = Ref<StubRenderPass>::Create("A");
    a->SetName("A");
    graph.AddNode(a.As<RenderGraphNode>());

    graph.ResetTopology();

    ASSERT_NE(a.Raw(), nullptr);
    EXPECT_EQ(a->GetName(), "A");

    // Re-registering the same pass instance is legal — simulates the
    // per-path rebuild re-adding persistent graph entries.
    graph.AddNode(a.As<RenderGraphNode>());
    graph.SetFinalPass("A");
    graph.Execute();
    EXPECT_EQ(a->GetExecuteCount(), 1u);
}

TEST(RenderGraphResetTopology, MultipleResetsAreSafe)
{
    // Rebuilding repeatedly (e.g. user toggling RenderingPath rapidly)
    // must not accumulate state or crash.
    RenderGraph graph;
    for (i32 i = 0; i < 5; ++i)
    {
        graph.ResetTopology();
        AddStub(graph, "P");
        graph.SetFinalPass("P");
        graph.Execute();
        EXPECT_EQ(graph.GetNodeSubmissionInfo().size(), 1u);
    }
}

TEST(RenderGraphResetTopology, ImportedHandleSlotsAreRebackedAfterReset)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ImportedTexture");
    const auto oldTexture = graph.ImportTexture("ImportedTexture", 42u, textureDesc);
    EXPECT_EQ(graph.ResolveTexture(oldTexture), 42u);

    auto bufferDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::UniformBuffer, "ImportedBuffer");
    const auto oldBuffer = graph.ImportBuffer("ImportedBuffer", 77u, bufferDesc);
    EXPECT_EQ(graph.ResolveBuffer(oldBuffer), 77u);

    auto framebufferDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "ImportedFramebuffer");
    auto oldFramebufferRef = Ref<StubFramebuffer>::Create(10u);
    const auto oldFramebuffer = graph.ImportFramebuffer("ImportedFramebuffer", oldFramebufferRef, framebufferDesc);
    EXPECT_EQ(graph.ResolveFramebuffer(oldFramebuffer).Raw(), oldFramebufferRef.Raw());

    graph.ResetTopology();

    EXPECT_FALSE(graph.IsTextureHandleCurrent(oldTexture));
    EXPECT_FALSE(graph.IsBufferHandleCurrent(oldBuffer));
    EXPECT_FALSE(graph.IsFramebufferHandleCurrent(oldFramebuffer));

    const auto newTexture = graph.ImportTexture("ImportedTexture", 99u, textureDesc);
    const auto newBuffer = graph.ImportBuffer("ImportedBuffer", 123u, bufferDesc);
    auto newFramebufferRef = Ref<StubFramebuffer>::Create(20u);
    const auto newFramebuffer = graph.ImportFramebuffer("ImportedFramebuffer", newFramebufferRef, framebufferDesc);

    EXPECT_TRUE(graph.IsTextureHandleCurrent(newTexture));
    EXPECT_TRUE(graph.IsBufferHandleCurrent(newBuffer));
    EXPECT_TRUE(graph.IsFramebufferHandleCurrent(newFramebuffer));
    EXPECT_EQ(graph.ResolveTexture(newTexture), 99u);
    EXPECT_EQ(graph.ResolveBuffer(newBuffer), 123u);
    EXPECT_EQ(graph.ResolveFramebuffer(newFramebuffer).Raw(), newFramebufferRef.Raw());
}

TEST(RenderGraphResetTopology, ClearsGraphNodeBuildDeclarations)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "NodePass",
        [](RGBuilder& builder)
        {
            auto nodeResource = builder.ImportTexture(
                "NodeResetSentinel",
                17u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "NodeResetSentinel"));
            builder.Write(nodeResource, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("NodePass");
    graph.BuildFrameGraph();
    ASSERT_NE(graph.FindRegisteredResource("NodeResetSentinel"), nullptr)
        << "Initial node build should contribute its declared resource";

    graph.ResetTopology();

    AddSetupNode(
        graph,
        "NodePass",
        [](RGBuilder& /*builder*/)
        {
            // Rebuilt topology does not declare the sentinel this frame.
        });

    graph.SetFinalPass("NodePass");
    graph.BuildFrameGraph();

    EXPECT_EQ(graph.FindRegisteredResource("NodeResetSentinel"), nullptr)
        << "ResetTopology must drop stale node-owned builder declarations so rebuilt paths only see current-frame resources";
}

// =============================================================================
// TransientPool plumbing — non-GL lifecycle coverage
// =============================================================================

TEST(RenderGraphTransientPool, StartsEmptyAndReportsZeroMemory)
{
    TransientPool pool;

    const auto stats = pool.GetStats();
    EXPECT_EQ(stats.TexturePoolSize, 0u);
    EXPECT_EQ(stats.TextureAliasGroups, 0u);
    EXPECT_EQ(stats.FramebufferPoolSize, 0u);
    EXPECT_EQ(stats.FramebufferAliasGroups, 0u);
    EXPECT_EQ(stats.BufferPoolSize, 0u);
    EXPECT_EQ(stats.BufferAliasGroups, 0u);
    EXPECT_EQ(pool.EstimateMemoryUsage(), 0u);

    EXPECT_NO_THROW(pool.LogStats());
}

TEST(RenderGraphTransientPool, ResetAndShutdownKeepPoolEmpty)
{
    RenderGraph graph;

    EXPECT_EQ(graph.GetTransientPool().EstimateMemoryUsage(), 0u);

    graph.ResetTopology();

    auto resetStats = graph.GetTransientPool().GetStats();
    EXPECT_EQ(resetStats.TexturePoolSize, 0u);
    EXPECT_EQ(resetStats.FramebufferPoolSize, 0u);
    EXPECT_EQ(resetStats.BufferPoolSize, 0u);
    EXPECT_EQ(graph.GetTransientPool().EstimateMemoryUsage(), 0u);

    graph.Shutdown();

    const auto shutdownStats = graph.GetTransientPool().GetStats();
    EXPECT_EQ(shutdownStats.TexturePoolSize, 0u);
    EXPECT_EQ(shutdownStats.FramebufferPoolSize, 0u);
    EXPECT_EQ(shutdownStats.BufferPoolSize, 0u);
    EXPECT_EQ(graph.GetTransientPool().EstimateMemoryUsage(), 0u);
}

TEST(RenderGraphTransientPool, ResizeEvictsStalePoolEntries)
{
    // Regression for the viewport-resize leak: TransientPool keys its buckets
    // on the full framebuffer spec — including width/height — so post-resize
    // Acquire calls miss into fresh buckets while the old ones sit forever,
    // both leaking GPU memory and letting the alias-group resolver hand a
    // stale-size sibling to a downstream pass (which then blits old-size
    // content into the new-size target → visible "duplicated, offset"
    // ghost geometry exactly as reported on master 2026-05-25).
    //
    // We populate the pool with real Acquire+ReleaseAll cycles (which
    // requires a live GL backend) so the post-Resize stats actually
    // discriminate the bug: with Clear() removed from RenderGraph::Resize,
    // those buckets would survive the dimension change.

    if (RendererAPI::GetAPI() == RendererAPI::API::None)
        GTEST_SKIP() << "TransientPool resize eviction requires an active rendering backend";

    OloEngine::Tests::RenderPropertyFixture::IsGpuAvailable();
    OLO_ENSURE_GPU_OR_SKIP();

    RenderGraph graph;

    // Seed the graph's physical dimensions so the no-op early-return in
    // Resize() doesn't fire on the test's initial call. (Same-dimension
    // resizes intentionally skip the Clear to avoid churn on idle frames.)
    graph.Resize(640, 360);
    graph.SetTransientPoolMaxBucketSize(2u);

    // Populate both pool buckets via real Acquire calls, then ReleaseAll
    // so the resources are returned to their per-spec buckets (Acquire
    // pulls from the bucket into m_AcquiredX; only ReleaseAll moves them
    // back to the bucket where GetStats can see them).
    {
        TransientPool& pool = graph.GetTransientPool();

        TextureSpecification texSpec;
        texSpec.Width = 640;
        texSpec.Height = 360;
        texSpec.Format = ImageFormat::RGBA8;
        (void)pool.AcquireTexture(texSpec);

        FramebufferSpecification fbSpec;
        fbSpec.Width = 640;
        fbSpec.Height = 360;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        (void)pool.AcquireFramebuffer(fbSpec);

        pool.ReleaseAll();
    }

    const auto before = graph.GetTransientPool().GetStats();
    EXPECT_GT(before.FramebufferPoolSize, 0u)
        << "Sanity: ReleaseAll should have parked the framebuffer in its bucket.";
    EXPECT_GT(before.TexturePoolSize, 0u)
        << "Sanity: ReleaseAll should have parked the texture in its bucket.";

    graph.Resize(1280, 720); // dimensions changed → must invoke Clear()

    const auto after = graph.GetTransientPool().GetStats();
    EXPECT_EQ(after.FramebufferPoolSize, 0u)
        << "Resize with new dimensions must leave the transient framebuffer pool empty.";
    EXPECT_EQ(after.TexturePoolSize, 0u)
        << "Resize with new dimensions must leave the transient texture pool empty.";
    EXPECT_EQ(after.BufferPoolSize, 0u);

    // A no-op resize (same dimensions) must not churn the pool — it's a
    // common case during idle frames where the editor recomputes the
    // viewport size and re-emits the existing values. Re-populate the
    // pool at the post-resize dimensions so the assertion can actually
    // discriminate eviction from a steady state.
    {
        TransientPool& pool = graph.GetTransientPool();

        TextureSpecification texSpec;
        texSpec.Width = 1280;
        texSpec.Height = 720;
        texSpec.Format = ImageFormat::RGBA8;
        (void)pool.AcquireTexture(texSpec);

        FramebufferSpecification fbSpec;
        fbSpec.Width = 1280;
        fbSpec.Height = 720;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        (void)pool.AcquireFramebuffer(fbSpec);

        pool.ReleaseAll();
    }

    const auto beforeNoOp = graph.GetTransientPool().GetStats();
    ASSERT_GT(beforeNoOp.FramebufferPoolSize, 0u)
        << "Sanity: re-populated framebuffer pool should be non-empty before the no-op resize.";
    ASSERT_GT(beforeNoOp.TexturePoolSize, 0u)
        << "Sanity: re-populated texture pool should be non-empty before the no-op resize.";

    graph.Resize(1280, 720);
    const auto idle = graph.GetTransientPool().GetStats();
    EXPECT_EQ(idle.FramebufferPoolSize, beforeNoOp.FramebufferPoolSize)
        << "Same-dimension resize must not churn the framebuffer pool.";
    EXPECT_EQ(idle.TexturePoolSize, beforeNoOp.TexturePoolSize)
        << "Same-dimension resize must not churn the texture pool.";
    EXPECT_EQ(idle.BufferPoolSize, beforeNoOp.BufferPoolSize);
}

TEST(RenderGraphTransientPool, UnreachableTransientResourceIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "TransientProducer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 1280;
            desc.Height = 720;
            desc.DebugName = "UnusedTransient";

            const auto temp = builder.CreateTexture("UnusedTransient", desc);
            builder.Write(temp, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "FinalColorTex",
                9,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "UnusedTransient";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_FALSE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "unreachable-or-disabled");
}

TEST(RenderGraphTransientPool, NonOverlappingTransientResourcesReuseAliasSlot)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "A",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 960;
            desc.Height = 540;

            const auto tempA = builder.CreateTexture("TempA", desc);
            builder.Write(tempA, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "B",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 960;
            desc.Height = 540;

            const auto tempA = builder.CreateTexture("TempA", desc);
            [[maybe_unused]] const auto readTempA = builder.Read(tempA, RGReadUsage::ShaderSample);

            auto sceneColor = builder.ImportTexture(
                "SceneColorTex",
                15,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColorTex"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "C",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 960;
            desc.Height = 540;

            const auto tempB = builder.CreateTexture("TempB", desc);
            builder.Write(tempB, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 960;
            desc.Height = 540;

            const auto tempB = builder.CreateTexture("TempB", desc);
            [[maybe_unused]] const auto readTempB = builder.Read(tempB, RGReadUsage::ShaderSample);
        });

    // Keep TempA chain reachable from Final so both transient candidates
    // participate in planning and alias-slot assignment.
    graph.AddExecutionDependency("B", "C");

    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto findPlan = [&transientPlan](const std::string& resource) -> const RenderGraph::TransientPlanEntry*
    {
        const auto it = std::ranges::find_if(transientPlan,
                                             [&resource](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == resource;
                                             });
        return it == transientPlan.end() ? nullptr : &(*it);
    };

    const auto* tempA = findPlan("TempA");
    const auto* tempB = findPlan("TempB");

    ASSERT_NE(tempA, nullptr);
    ASSERT_NE(tempB, nullptr);
    EXPECT_TRUE(tempA->WillAllocate);
    EXPECT_TRUE(tempB->WillAllocate);
    EXPECT_EQ(tempA->AliasGroup, tempB->AliasGroup);
    EXPECT_EQ(tempA->AliasSlot, tempB->AliasSlot)
        << "Non-overlapping lifetimes with identical descriptors should reuse alias slot";
    EXPECT_LT(tempA->LastPassIndex, tempB->FirstPassIndex)
        << "Test setup requires non-overlapping lifetimes for alias reuse";
}

// Phase D: RG16Float is now a supported framebuffer format (maps to RG16F attachment).
// R8UNorm has no framebuffer equivalent and remains the canonical "unsupported" test case.
TEST(RenderGraphTransientPool, UnsupportedFramebufferFormatIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::R8UNorm; // No single-channel non-integer FB format
            desc.Width = 640;
            desc.Height = 360;

            const auto unsupported = builder.CreateFramebuffer("UnsupportedTransientFB", desc);
            builder.Write(unsupported, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::R8UNorm;
            desc.Width = 640;
            desc.Height = 360;

            const auto unsupported = builder.CreateFramebuffer("UnsupportedTransientFB", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::RenderTargetRead);
        });

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "UnsupportedTransientFB";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "unsupported-framebuffer-format");
}

// Phase D: RG16F framebuffer format is now a supported transient FB type.
TEST(RenderGraphTransientPool, PhaseD_RG16FFramebufferFormatIsNowAllocatable)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Pass",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RG16Float;
            desc.Width = 640;
            desc.Height = 360;

            const auto fb = builder.CreateFramebuffer("PhaseD_RG16FFB", desc);
            builder.Write(fb, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto r = builder.Read(fb, RGReadUsage::RenderTargetRead);
        });

    graph.SetFinalPass("Pass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "PhaseD_RG16FFB"; });

    ASSERT_NE(it, plan.end());
    EXPECT_TRUE(it->WillAllocate) << "RG16F FB must be allocatable after Phase D fix";
    EXPECT_EQ(it->SkipReason, "") << "unexpected skip reason: " << it->SkipReason;
}

// Phase D: SSAOPass-style setup that declares SSAORaw as a transient RG16F FB.
// Verifies the graph accepts it and plans allocation (no GPU context required).
TEST(RenderGraphTransientPool, PhaseD_SSAOPassDeclaresTransientRawFramebuffer)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "SSAOPass",
        [](RGBuilder& builder)
        {
            // Declare SSAORaw — mirrors the production setup callback in Renderer3D::ConfigureRenderGraph
            RGResourceDesc rawDesc;
            rawDesc.Kind = ResourceHandle::Kind::Framebuffer;
            rawDesc.Format = RGResourceFormat::RG16Float;
            rawDesc.Width = 698; // typical half-res of 1396-wide viewport
            rawDesc.Height = 418;

            const auto rawHandle = builder.CreateFramebuffer("SSAORaw", rawDesc);
            builder.Write(rawHandle, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto rawRead = builder.Read(rawHandle, RGReadUsage::RenderTargetRead);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("SSAOPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    // SSAORaw should be reachable and planned for allocation
    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "SSAORaw"; });

    ASSERT_NE(it, plan.end()) << "SSAORaw not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "SSAORaw must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "SSAORaw must be planned for allocation (Phase D fix)";
    EXPECT_EQ(it->SkipReason, "") << "unexpected skip reason: " << it->SkipReason;

    // The stable handle must be resolvable after BuildFrameGraph
    const auto handle = graph.GetFramebufferHandle("SSAORaw");
    EXPECT_TRUE(handle.IsValid()) << "stable handle for SSAORaw must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_SSAOAOBufferDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc aoDesc;
    aoDesc.Kind = ResourceHandle::Kind::Texture2D;
    aoDesc.Format = RGResourceFormat::RG16Float;
    aoDesc.Width = 698;
    aoDesc.Height = 418;
    const auto aoHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::AOBuffer), aoDesc);

    AddSetupNode(
        graph,
        "SSAOPass",
        [aoHandle](RGBuilder& builder)
        {
            builder.Write(aoHandle, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "AOApplyPass",
        [aoHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto aoRead = builder.Read(aoHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("SSAOPass", "AOApplyPass");
    graph.SetFinalPass("AOApplyPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::AOBuffer; });

    ASSERT_NE(it, plan.end()) << "AOBuffer not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "AOBuffer must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "AOBuffer must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "AOBuffer unexpected skip reason: " << it->SkipReason;
    EXPECT_EQ(it->EstimatedBytes, 698ull * 418ull * 4ull)
        << "AOBuffer (RG16F) should be 4 bytes per texel";

    const auto handle = graph.GetTextureHandle(std::string(ResourceNames::AOBuffer));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for AOBuffer must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_SSAOBlurFramebufferDeclaredAsTransientFramebuffer)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc blurDesc;
    blurDesc.Kind = ResourceHandle::Kind::Framebuffer;
    blurDesc.Format = RGResourceFormat::RG16Float;
    blurDesc.Width = 698;
    blurDesc.Height = 418;
    const auto blurHandle = graph.DeclareTransientFramebuffer(std::string(ResourceNames::SSAOBlur), blurDesc);

    AddSetupNode(
        graph,
        "SSAOPass",
        [blurHandle](RGBuilder& builder)
        {
            builder.Write(blurHandle, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("SSAOPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::SSAOBlur; });

    ASSERT_NE(it, plan.end()) << "SSAOBlur not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "SSAOBlur must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "SSAOBlur must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "SSAOBlur unexpected skip reason: " << it->SkipReason;

    const auto handle = graph.GetFramebufferHandle(std::string(ResourceNames::SSAOBlur));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for SSAOBlur must be valid after BuildFrameGraph";
}

// Phase D Slice 2: RGBA32F framebuffer format is now a supported transient FB type.
TEST(RenderGraphTransientPool, PhaseD_RGBA32FFramebufferFormatIsAllocatable)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Pass",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RGBA32Float;
            desc.Width = 1280;
            desc.Height = 720;

            const auto fb = builder.CreateFramebuffer("PhaseD_RGBA32FFB", desc);
            builder.Write(fb, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto r = builder.Read(fb, RGReadUsage::RenderTargetRead);
        });

    graph.SetFinalPass("Pass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "PhaseD_RGBA32FFB"; });

    ASSERT_NE(it, plan.end());
    EXPECT_TRUE(it->WillAllocate) << "RGBA32F FB must be allocatable after Phase D Slice 2 fix";
    EXPECT_EQ(it->SkipReason, "") << "unexpected skip reason: " << it->SkipReason;
}

// Phase D Slice 2: SelectionOutline-style setup declaring JFAPing + JFAPong
// as full-res RGBA32F transient framebuffers.
TEST(RenderGraphTransientPool, PhaseD_SelectionOutlinePassDeclaresPingPongJFAFramebuffers)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "SelectionOutlinePass",
        [](RGBuilder& builder)
        {
            // Mirror the production setup callback in Renderer3D::ConfigureRenderGraph
            RGResourceDesc jfaDesc;
            jfaDesc.Kind = ResourceHandle::Kind::Framebuffer;
            jfaDesc.Format = RGResourceFormat::RGBA32Float;
            jfaDesc.Width = 1280;
            jfaDesc.Height = 720;

            const auto pingHandle = builder.CreateFramebuffer("JFAPing", jfaDesc);
            builder.Write(pingHandle, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto pingRead = builder.Read(pingHandle, RGReadUsage::RenderTargetRead);

            const auto pongHandle = builder.CreateFramebuffer("JFAPong", jfaDesc);
            builder.Write(pongHandle, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto pongRead = builder.Read(pongHandle, RGReadUsage::RenderTargetRead);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("SelectionOutlinePass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();

    for (const char* name : { "JFAPing", "JFAPong" })
    {
        const auto it = std::ranges::find_if(plan,
                                             [name](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == name; });

        ASSERT_NE(it, plan.end()) << name << " not found in transient plan";
        EXPECT_TRUE(it->Reachable) << name << " must be reachable";
        EXPECT_TRUE(it->WillAllocate) << name << " must be planned for allocation (Phase D Slice 2)";
        EXPECT_EQ(it->SkipReason, "") << name << " unexpected skip reason: " << it->SkipReason;

        const auto handle = graph.GetFramebufferHandle(name);
        EXPECT_TRUE(handle.IsValid()) << "stable handle for " << name << " must be valid after BuildFrameGraph";
    }

    // Both JFA framebuffers have identical descriptors — they should alias into
    // the same alias slot (non-overlapping lifetimes are not guaranteed here,
    // but at minimum both should be planned).
    const auto pingIt = std::ranges::find_if(plan,
                                             [](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == "JFAPing"; });
    const auto pongIt = std::ranges::find_if(plan,
                                             [](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == "JFAPong"; });
    ASSERT_NE(pingIt, plan.end());
    ASSERT_NE(pongIt, plan.end());
    // Both must report the same descriptor (same size + format = alias-compatible)
    EXPECT_EQ(pingIt->EstimatedBytes, pongIt->EstimatedBytes)
        << "JFAPing and JFAPong have the same desc — should report equal estimated sizes";
}

// Phase D Slice 3: Bloom mip-chain setup declaring up to 5 RGBA16F scratch FBs.
TEST(RenderGraphTransientPool, PhaseD_BloomMipChainDeclaredAsTransientFramebuffers)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    // Mirror the production setup callback in Renderer3D::ConfigureRenderGraph.
    // Using viewport 1280x720 → mip dims: 640x360, 320x180, 160x90, 80x45, 40x22.
    AddSetupNode(
        graph,
        "BloomPass",
        [](RGBuilder& builder)
        {
            u32 mipW = 1280 / 2;
            u32 mipH = 720 / 2;
            for (u32 i = 0; i < 5u; ++i)
            {
                if (mipW < 2 || mipH < 2)
                    break;
                RGResourceDesc mipDesc;
                mipDesc.Kind = ResourceHandle::Kind::Framebuffer;
                mipDesc.Format = RGResourceFormat::RGBA16Float;
                mipDesc.Width = mipW;
                mipDesc.Height = mipH;
                const std::string mipName = "BloomMip" + std::to_string(i);
                const auto mipHandle = builder.CreateFramebuffer(mipName, mipDesc);
                builder.Write(mipHandle, RGWriteUsage::RenderTarget);
                [[maybe_unused]] const auto mipRead =
                    builder.Read(mipHandle, RGReadUsage::RenderTargetRead);
                mipW /= 2;
                mipH /= 2;
            }
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("BloomPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();

    // All 5 mip levels should appear in the transient plan with allocation planned.
    for (u32 i = 0; i < 5u; ++i)
    {
        const std::string mipName = "BloomMip" + std::to_string(i);
        const auto it = std::ranges::find_if(plan,
                                             [&mipName](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == mipName; });

        ASSERT_NE(it, plan.end()) << mipName << " not found in transient plan";
        EXPECT_TRUE(it->Reachable) << mipName << " must be reachable";
        EXPECT_TRUE(it->WillAllocate) << mipName << " must be planned for allocation (Phase D Slice 3)";
        EXPECT_EQ(it->SkipReason, "") << mipName << " unexpected skip reason: " << it->SkipReason;

        const auto handle = graph.GetFramebufferHandle(mipName);
        EXPECT_TRUE(handle.IsValid()) << "stable handle for " << mipName << " must be valid after BuildFrameGraph";
    }

    // Mip 0 should be larger than mip 1 (halving each level).
    const auto mip0It = std::ranges::find_if(plan,
                                             [](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == "BloomMip0"; });
    const auto mip1It = std::ranges::find_if(plan,
                                             [](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == "BloomMip1"; });
    ASSERT_NE(mip0It, plan.end());
    ASSERT_NE(mip1It, plan.end());
    EXPECT_GT(mip0It->EstimatedBytes, mip1It->EstimatedBytes)
        << "BloomMip0 (640x360) should be larger than BloomMip1 (320x180)";
}

// Phase D Slice 4: GTAO edge scratch texture declared as transient R8 resource.
TEST(RenderGraphTransientPool, PhaseD_GTAOEdgeTextureDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GTAOPass",
        [](RGBuilder& builder)
        {
            // Mirror the production setup callback in Renderer3D::ConfigureRenderGraph.
            RGResourceDesc edgeDesc;
            edgeDesc.Kind = ResourceHandle::Kind::Texture2D;
            edgeDesc.Format = RGResourceFormat::R8UNorm;
            edgeDesc.Width = 1280;
            edgeDesc.Height = 720;
            const auto edgeHandle = builder.CreateTexture("GTAOEdge", edgeDesc);
            builder.Write(edgeHandle, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto edgeRead =
                builder.Read(edgeHandle, RGReadUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("GTAOPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "GTAOEdge"; });

    ASSERT_NE(it, plan.end()) << "GTAOEdge not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "GTAOEdge must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "GTAOEdge must be planned for allocation (Phase D Slice 4)";
    EXPECT_EQ(it->SkipReason, "") << "GTAOEdge unexpected skip reason: " << it->SkipReason;
    // R8 = 1 byte per texel
    EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 1ull)
        << "GTAOEdge (R8) should be 1 byte per texel";

    const auto handle = graph.GetTextureHandle("GTAOEdge");
    EXPECT_TRUE(handle.IsValid()) << "stable handle for GTAOEdge must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_GTAOAOBufferDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc aoDesc;
    aoDesc.Kind = ResourceHandle::Kind::Texture2D;
    aoDesc.Format = RGResourceFormat::R8UNorm;
    aoDesc.Width = 1280;
    aoDesc.Height = 720;
    const auto aoHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::AOBuffer), aoDesc);

    AddSetupNode(
        graph,
        "GTAOPass",
        [aoHandle](RGBuilder& builder)
        {
            builder.Write(aoHandle, RGWriteUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "AOApplyPass",
        [aoHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto aoRead = builder.Read(aoHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("GTAOPass", "AOApplyPass");
    graph.SetFinalPass("AOApplyPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::AOBuffer; });

    ASSERT_NE(it, plan.end()) << "AOBuffer not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "AOBuffer must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "AOBuffer must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "AOBuffer unexpected skip reason: " << it->SkipReason;
    EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 1ull)
        << "AOBuffer (R8) should be 1 byte per texel";

    const auto handle = graph.GetTextureHandle(std::string(ResourceNames::AOBuffer));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for AOBuffer must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_GTAODenoisePingPongDeclaredAsTransientTextures)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc denoiseDesc;
    denoiseDesc.Kind = ResourceHandle::Kind::Texture2D;
    denoiseDesc.Format = RGResourceFormat::R8UNorm;
    denoiseDesc.Width = 1280;
    denoiseDesc.Height = 720;
    const auto pingHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::GTAODenoisePing), denoiseDesc);
    const auto pongHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::GTAODenoisePong), denoiseDesc);

    AddSetupNode(
        graph,
        "GTAOPass",
        [pingHandle, pongHandle](RGBuilder& builder)
        {
            builder.AllowSamePassReadWrite(pingHandle);
            builder.Write(pingHandle, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto pingRead = builder.Read(pingHandle, RGReadUsage::ShaderImage);

            builder.AllowSamePassReadWrite(pongHandle);
            builder.Write(pongHandle, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto pongRead = builder.Read(pongHandle, RGReadUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("GTAOPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    for (const auto resourceName : { ResourceNames::GTAODenoisePing, ResourceNames::GTAODenoisePong })
    {
        const auto it = std::ranges::find_if(plan,
                                             [resourceName](const RenderGraph::TransientPlanEntry& entry)
                                             { return entry.Resource == resourceName; });

        ASSERT_NE(it, plan.end()) << resourceName << " not found in transient plan";
        EXPECT_TRUE(it->Reachable) << resourceName << " must be reachable";
        EXPECT_TRUE(it->WillAllocate) << resourceName << " must be planned for allocation";
        EXPECT_EQ(it->SkipReason, "") << resourceName << " unexpected skip reason: " << it->SkipReason;
        EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 1ull)
            << resourceName << " (R8) should be 1 byte per texel";
    }

    EXPECT_TRUE(graph.GetTextureHandle(std::string(ResourceNames::GTAODenoisePing)).IsValid())
        << "stable handle for GTAODenoisePing must be valid after BuildFrameGraph";
    EXPECT_TRUE(graph.GetTextureHandle(std::string(ResourceNames::GTAODenoisePong)).IsValid())
        << "stable handle for GTAODenoisePong must be valid after BuildFrameGraph";
}

// Phase D Slice 6: HZB scratch depth pyramid declared as transient R32F mip-chain texture.
TEST(RenderGraphTransientPool, PhaseD_HZBDepthDeclaredAsTransientMipChainTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GTAOPass",
        [](RGBuilder& builder)
        {
            // Mirror the production setup callback in Renderer3D::ConfigureRenderGraph.
            // Viewport 1396x835 -> HZB 2048x1024 with 12 mips.
            RGResourceDesc hzbDesc;
            hzbDesc.Kind = ResourceHandle::Kind::Texture2D;
            hzbDesc.Format = RGResourceFormat::R32Float;
            hzbDesc.Width = 2048;
            hzbDesc.Height = 1024;
            hzbDesc.MipLevels = 12;

            const auto hzbHandle = builder.CreateTexture("HZBDepth", hzbDesc);
            builder.Write(hzbHandle, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto hzbRead = builder.Read(hzbHandle, RGReadUsage::ShaderSample);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("GTAOPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "HZBDepth"; });

    ASSERT_NE(it, plan.end()) << "HZBDepth not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "HZBDepth must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "HZBDepth must be planned for allocation (Phase D Slice 6)";
    EXPECT_EQ(it->SkipReason, "") << "HZBDepth unexpected skip reason: " << it->SkipReason;
    EXPECT_NE(it->AliasGroup.find(":m12:"), std::string::npos)
        << "Alias group should encode mip count for HZBDepth mip chain";
    EXPECT_EQ(it->EstimatedBytes, 2048ull * 1024ull * 4ull * 12ull)
        << "HZBDepth (R32F, 12 mips) estimated bytes should include mip multiplier in current planner model";

    const auto handle = graph.GetTextureHandle("HZBDepth");
    EXPECT_TRUE(handle.IsValid()) << "stable handle for HZBDepth must be valid after BuildFrameGraph";
}

// Phase D Slice 5: Water refraction scratch texture declared as transient RGBA16F resource.
TEST(RenderGraphTransientPool, PhaseD_WaterRefractionDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "WaterPass",
        [](RGBuilder& builder)
        {
            // Mirror the production setup callback in Renderer3D::ConfigureRenderGraph.
            RGResourceDesc refrDesc;
            refrDesc.Kind = ResourceHandle::Kind::Texture2D;
            refrDesc.Format = RGResourceFormat::RGBA16Float;
            refrDesc.Width = 1280;
            refrDesc.Height = 720;
            const auto refrHandle = builder.CreateTexture("WaterRefraction", refrDesc);
            builder.Write(refrHandle, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto refrRead =
                builder.Read(refrHandle, RGReadUsage::ShaderSample);
        });

    AddSetupNode(
        graph,
        "FinalPass",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("WaterPass", "FinalPass");
    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "WaterRefraction"; });

    ASSERT_NE(it, plan.end()) << "WaterRefraction not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "WaterRefraction must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "WaterRefraction must be planned for allocation (Phase D Slice 5)";
    EXPECT_EQ(it->SkipReason, "") << "WaterRefraction unexpected skip reason: " << it->SkipReason;
    // RGBA16F = 8 bytes per texel
    EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 8ull)
        << "WaterRefraction (RGBA16F) should be 8 bytes per texel";

    const auto handle = graph.GetTextureHandle("WaterRefraction");
    EXPECT_TRUE(handle.IsValid()) << "stable handle for WaterRefraction must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, RG16FloatTextureIsPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RG16Float;
            desc.Width = 320;
            desc.Height = 180;

            const auto velocity = builder.CreateTexture("VelocityTransient", desc);
            builder.Write(velocity, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RG16Float;
            desc.Width = 320;
            desc.Height = 180;

            const auto velocity = builder.CreateTexture("VelocityTransient", desc);
            [[maybe_unused]] const auto readVelocity = builder.Read(velocity, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "VelocityTransient";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_TRUE(planIt->WillAllocate);
    EXPECT_TRUE(planIt->SkipReason.empty());
    EXPECT_EQ(planIt->EstimatedBytes, 320ull * 180ull * 4ull);
}

TEST(RenderGraphTransientPool, UnsupportedImageFormatIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::Unknown;
            desc.Width = 320;
            desc.Height = 180;

            const auto unsupported = builder.CreateTexture("UnsupportedTransientTex", desc);
            builder.Write(unsupported, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::Unknown;
            desc.Width = 320;
            desc.Height = 180;

            const auto unsupported = builder.CreateTexture("UnsupportedTransientTex", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "UnsupportedTransientTex";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "unknown-format");
}

TEST(RenderGraphTransientPool, MissingDimensionsTextureIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 0;
            desc.Height = 180;

            const auto unsupported = builder.CreateTexture("MissingDimensionsTex", desc);
            builder.Write(unsupported, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 0;
            desc.Height = 180;

            const auto unsupported = builder.CreateTexture("MissingDimensionsTex", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "MissingDimensionsTex";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "missing-dimensions");
}

TEST(RenderGraphTransientPool, MissingDimensionsFramebufferIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 640;
            desc.Height = 0;

            const auto unsupported = builder.CreateFramebuffer("MissingDimensionsFB", desc);
            builder.Write(unsupported, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 640;
            desc.Height = 0;

            const auto unsupported = builder.CreateFramebuffer("MissingDimensionsFB", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::RenderTargetRead);
        });

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "MissingDimensionsFB";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "missing-dimensions");
}

TEST(RenderGraphTransientPool, ZeroSizeTransientBufferIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::StorageBuffer;
            desc.Width = 0;

            const auto unsupported = builder.CreateBuffer("ZeroByteTransientBuffer", desc);
            builder.Write(unsupported, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::StorageBuffer;
            desc.Width = 0;

            const auto unsupported = builder.CreateBuffer("ZeroByteTransientBuffer", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::ShaderStorage);
        });

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "ZeroByteTransientBuffer";
                                             });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "zero-size-buffer");
}

TEST(RenderGraphTransientPool, DumpToJsonIncludesTransientAliasDiagnostics)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 256;
            desc.Height = 256;

            const auto tempA = builder.CreateTexture("JsonTempA", desc);
            builder.Write(tempA, RGWriteUsage::RenderTarget);

            RGResourceDesc invalidDesc;
            invalidDesc.Kind = ResourceHandle::Kind::Framebuffer;
            // Phase D: RG16Float is now supported. Use R8UNorm — it has no FB equivalent
            // and remains the canonical "unsupported-framebuffer-format" case.
            invalidDesc.Format = RGResourceFormat::R8UNorm;
            invalidDesc.Width = 256;
            invalidDesc.Height = 256;

            const auto invalid = builder.CreateFramebuffer("JsonInvalidFB", invalidDesc);
            builder.Write(invalid, RGWriteUsage::RenderTarget);

            const auto unreachable = builder.CreateTexture("JsonUnused", desc);
            builder.Write(unreachable, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Consumer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 256;
            desc.Height = 256;

            const auto tempA = builder.CreateTexture("JsonTempA", desc);
            [[maybe_unused]] const auto readTempA = builder.Read(tempA, RGReadUsage::ShaderSample);

            const auto tempB = builder.CreateTexture("JsonTempB", desc);
            builder.Write(tempB, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 256;
            desc.Height = 256;

            const auto tempB = builder.CreateTexture("JsonTempB", desc);
            [[maybe_unused]] const auto readTempB = builder.Read(tempB, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Producer", "Consumer");
    graph.AddExecutionDependency("Consumer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_transient_alias_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());

    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find("\"aliases\""), std::string::npos);
    EXPECT_NE(json.find("JsonTempA"), std::string::npos);
    EXPECT_NE(json.find("JsonTempB"), std::string::npos);
    EXPECT_NE(json.find("JsonInvalidFB"), std::string::npos);
    EXPECT_NE(json.find("JsonUnused"), std::string::npos);
    EXPECT_NE(json.find("unsupported-framebuffer-format"), std::string::npos);
    EXPECT_NE(json.find("\"willAllocate\": true"), std::string::npos);
    EXPECT_NE(json.find("\"resource\": \"JsonUnused\""), std::string::npos);
    EXPECT_NE(json.find("\"resource\": \"JsonUnused\", \"kind\": \"Texture2D\""), std::string::npos);
    EXPECT_NE(json.find("\"firstIndex\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"lastIndex\": 0"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraphTransientPool, MaterializationEnabledIsSafeForNonAllocatableTransient)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    AddSetupNode(
        graph,
        "TransientProducer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::Unknown;
            desc.Width = 320;
            desc.Height = 180;

            const auto temp = builder.CreateTexture("NonAllocatableTransient", desc);
            builder.Write(temp, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::Unknown;
            desc.Width = 320;
            desc.Height = 180;

            const auto temp = builder.CreateTexture("NonAllocatableTransient", desc);
            [[maybe_unused]] const auto readTemp = builder.Read(temp, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "NonAllocatableTransient";
                                             });

    ASSERT_NE(planIt, transientPlan.end()) << "Transient should exist in the plan";
    EXPECT_TRUE(planIt->Reachable) << "Transient should still participate in the frame graph";
    EXPECT_FALSE(planIt->WillAllocate) << "Unknown-format transient must not be materialized";
    EXPECT_EQ(planIt->SkipReason, "unknown-format");

    EXPECT_NO_THROW(graph.Execute());

    const auto handle = graph.GetTextureHandle("NonAllocatableTransient");
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(graph.ResolveTexture(handle), 0u)
        << "Non-allocatable transients must not create physical textures";
}

TEST(RenderGraphTransientPool, TransientTextureIsAllocatedFromPoolWhenMaterializationEnabled)
{
    // Skip test if no rendering backend is available
    if (RendererAPI::GetAPI() == RendererAPI::API::None)
        GTEST_SKIP() << "Transient materialization test requires an active rendering backend";

    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    // Producer pass: creates and writes to a transient texture
    AddSetupNode(
        graph,
        "TransientProducer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 256;
            desc.Height = 256;

            const auto transientColor = builder.CreateTexture("RuntimeTransientColor", desc);
            builder.Write(transientColor, RGWriteUsage::RenderTarget);
        });

    // Consumer pass: reads from the transient texture
    AddSetupNode(
        graph,
        "TransientConsumer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 256;
            desc.Height = 256;

            const auto transientColor = builder.CreateTexture("RuntimeTransientColor", desc);
            [[maybe_unused]] const auto readColor = builder.Read(transientColor, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("TransientConsumer");
    graph.BuildFrameGraph();
    // Verify the resource exists in the transient plan and was allocated
    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "RuntimeTransientColor";
                                             });

    ASSERT_NE(planIt, transientPlan.end()) << "Transient should exist in the plan";
    EXPECT_TRUE(planIt->Reachable) << "Transient should be reachable";
    EXPECT_TRUE(planIt->WillAllocate) << "Transient should be allocated from pool";
    EXPECT_TRUE(planIt->SkipReason.empty()) << "Reachable allocatable transient should have no skip reason";
}

TEST(RenderGraphTransientPool, MaterializedTransientExtractionReturnsValidTextureWithGpuContext)
{
    if (RendererAPI::GetAPI() == RendererAPI::API::None)
        GTEST_SKIP() << "Materialized transient extraction requires an active rendering backend";

    OLO_ENSURE_GPU_OR_SKIP();

    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    AddSetupNode(
        graph,
        "TransientWriter",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 128;
            desc.Height = 128;

            const auto transient = builder.CreateTexture("GpuReadbackTransient", desc);
            builder.Write(transient, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("TransientWriter");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "GpuReadbackTransient";
                                             });
    ASSERT_NE(planIt, transientPlan.end()) << "Transient should exist in the plan";
    EXPECT_TRUE(planIt->Reachable) << "Transient should be reachable";
    EXPECT_TRUE(planIt->WillAllocate) << "Transient should be marked allocatable in plan";

    const auto handle = graph.GetTextureHandle("GpuReadbackTransient");
    ASSERT_TRUE(handle.IsValid()) << "Transient handle must be valid after graph build";

    bool callbackCalled = false;
    u32 extractedTextureID = 0;
    graph.ExtractTexture(handle,
                         [&callbackCalled, &extractedTextureID](const u32 textureID)
                         {
                             callbackCalled = true;
                             extractedTextureID = textureID;
                         });

    EXPECT_NO_THROW(graph.Execute());
    EXPECT_TRUE(callbackCalled) << "Transient extraction callback must fire after Execute";
    EXPECT_NE(extractedTextureID, 0u)
        << "Materialized transient extraction must resolve a live texture ID when a GPU context is available";
}

TEST(RenderGraphTransientPool, NonAllocatableTransientExtractionInvokesCallbackWithZeroTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    AddSetupNode(
        graph,
        "TransientWriter",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::Unknown;
            desc.Width = 128;
            desc.Height = 128;

            const auto transient = builder.CreateTexture("NonAllocatableReadbackTransient", desc);
            builder.Write(transient, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("TransientWriter");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "NonAllocatableReadbackTransient";
                                             });
    ASSERT_NE(planIt, transientPlan.end()) << "Transient should exist in the plan";
    EXPECT_TRUE(planIt->Reachable) << "Transient should still participate in the frame graph";
    EXPECT_FALSE(planIt->WillAllocate) << "Unknown-format transient must not be materialized";
    EXPECT_EQ(planIt->SkipReason, "unknown-format");

    const auto handle = graph.GetTextureHandle("NonAllocatableReadbackTransient");
    ASSERT_TRUE(handle.IsValid()) << "Transient handle must be valid after graph build";

    bool callbackCalled = false;
    u32 extractedTextureID = 0;
    graph.ExtractTexture(handle,
                         [&callbackCalled, &extractedTextureID](const u32 textureID)
                         {
                             callbackCalled = true;
                             extractedTextureID = textureID;
                         });

    EXPECT_NO_THROW(graph.Execute());
    EXPECT_TRUE(callbackCalled) << "Transient extraction callback must fire after Execute";
    EXPECT_EQ(extractedTextureID, 0u)
        << "Headless transient extraction must resolve to texture ID 0 without a rendering backend";
}

// =============================================================================
// Phase D Slice 7 — OIT buffer as shared transient MRT framebuffer
// =============================================================================

// Verify that DeclareTransientFramebuffer registers a stable handle for a
// multi-attachment MRT descriptor before BuildFrameGraph is called.
TEST(RenderGraphTransientPool, PhaseD_DeclareTransientFramebufferReturnsValidHandleBeforeBuild)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc oitDesc;
    oitDesc.Kind = ResourceHandle::Kind::Framebuffer;
    oitDesc.Width = 1280;
    oitDesc.Height = 720;
    oitDesc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float, RGResourceFormat::Depth24Stencil8 };

    const auto handle = graph.DeclareTransientFramebuffer(ResourceNames::OITBuffer, oitDesc);
    EXPECT_TRUE(handle.IsValid())
        << "DeclareTransientFramebuffer must return a valid handle immediately";

    // GetFramebufferHandle must return the same stable handle.
    const auto namedHandle = graph.GetFramebufferHandle(ResourceNames::OITBuffer);
    EXPECT_TRUE(namedHandle.IsValid()) << "named lookup must also return a valid handle";
    EXPECT_EQ(handle.Index, namedHandle.Index)
        << "DeclareTransientFramebuffer and GetFramebufferHandle must return the same slot";
}

// Verify that an MRT OIT transient descriptor (including the graph-owned depth
// attachment) is planned for allocation when its resource is reachable.
TEST(RenderGraphTransientPool, PhaseD_OITBufferDeclaredAsSharedTransientMRTFramebuffer)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    // Register the OIT descriptor before BuildFrameGraph to mirror the
    // production SetupFrameBlackboard path.
    RGResourceDesc oitDesc;
    oitDesc.Kind = ResourceHandle::Kind::Framebuffer;
    oitDesc.Width = 1280;
    oitDesc.Height = 720;
    oitDesc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float, RGResourceFormat::Depth24Stencil8 };
    oitDesc.DebugName = std::string(ResourceNames::OITBuffer);

    const auto oitHandle = graph.DeclareTransientFramebuffer(ResourceNames::OITBuffer, oitDesc);
    ASSERT_TRUE(oitHandle.IsValid());

    AddSetupNode(
        graph,
        "OITWriterPass",
        [oitHandle](RGBuilder& builder)
        {
            builder.Write(oitHandle, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "OITResolvePass",
        [oitHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(oitHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("OITWriterPass", "OITResolvePass");
    graph.SetFinalPass("OITResolvePass");
    graph.BuildFrameGraph();

    // Resource must appear in the transient plan and be planned for allocation.
    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == std::string(ResourceNames::OITBuffer); });

    ASSERT_NE(it, plan.end()) << "OITBuffer not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "OITBuffer must be reachable";
    EXPECT_TRUE(it->WillAllocate)
        << "OITBuffer MRT must be planned for allocation; skip reason: " << it->SkipReason;
    EXPECT_EQ(it->SkipReason, "") << "unexpected skip reason: " << it->SkipReason;

    // EstimatedBytes must cover both color attachments plus depth:
    // RGBA16F (8 bytes) + RG16F (4 bytes) + DEPTH24_STENCIL8 (4 bytes) per pixel.
    const u64 expectedBytes = (8ull + 4ull + 4ull) * 1280ull * 720ull;
    EXPECT_EQ(it->EstimatedBytes, expectedBytes)
        << "MRT estimated bytes must sum across all color and depth attachments";

    // The shared OITBuffer framebuffer handle remains stable across build;
    // attachment views are layered on top separately.
    const auto afterBuildHandle = graph.GetFramebufferHandle(ResourceNames::OITBuffer);
    EXPECT_TRUE(afterBuildHandle.IsValid()) << "handle must still be valid after BuildFrameGraph";
    EXPECT_EQ(oitHandle.Index, afterBuildHandle.Index)
        << "stable handle index must not change between declaration and post-build lookup";
}

TEST(RenderGraphTransientPool, SameFrameImportsAndBuilderTransientsKeepStableHandles)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto importedSceneColor = graph.ImportFramebuffer(
        ResourceNames::SceneColor,
        nullptr,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, ResourceNames::SceneColor));
    const auto importedSceneDepth = graph.ImportTexture(
        ResourceNames::SceneDepth,
        17u,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, ResourceNames::SceneDepth));

    RGFramebufferHandle writerFramebufferHandle{};
    RGFramebufferHandle readerFramebufferHandle{};
    RGTextureHandle writerTextureHandle{};
    RGTextureHandle readerTextureHandle{};

    AddSetupNode(
        graph,
        "Writer",
        [&writerFramebufferHandle, &writerTextureHandle](RGBuilder& builder)
        {
            RGResourceDesc framebufferDesc;
            framebufferDesc.Kind = ResourceHandle::Kind::Framebuffer;
            framebufferDesc.Format = RGResourceFormat::RGBA16Float;
            framebufferDesc.Width = 640u;
            framebufferDesc.Height = 360u;

            writerFramebufferHandle = builder.CreateFramebuffer("StableScratchFB", framebufferDesc);
            builder.Write(writerFramebufferHandle, RGWriteUsage::RenderTarget);

            RGResourceDesc textureDesc;
            textureDesc.Kind = ResourceHandle::Kind::Texture2D;
            textureDesc.Format = RGResourceFormat::R8UNorm;
            textureDesc.Width = 640u;
            textureDesc.Height = 360u;

            writerTextureHandle = builder.CreateTexture("StableScratchTex", textureDesc);
            builder.Write(writerTextureHandle, RGWriteUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "Reader",
        [&readerFramebufferHandle, &readerTextureHandle](RGBuilder& builder)
        {
            RGResourceDesc framebufferDesc;
            framebufferDesc.Kind = ResourceHandle::Kind::Framebuffer;
            framebufferDesc.Format = RGResourceFormat::RGBA16Float;
            framebufferDesc.Width = 640u;
            framebufferDesc.Height = 360u;

            readerFramebufferHandle = builder.CreateFramebuffer("StableScratchFB", framebufferDesc);
            [[maybe_unused]] const auto scratchFramebufferRead =
                builder.Read(readerFramebufferHandle, RGReadUsage::RenderTargetRead);

            RGResourceDesc textureDesc;
            textureDesc.Kind = ResourceHandle::Kind::Texture2D;
            textureDesc.Format = RGResourceFormat::R8UNorm;
            textureDesc.Width = 640u;
            textureDesc.Height = 360u;

            readerTextureHandle = builder.CreateTexture("StableScratchTex", textureDesc);
            [[maybe_unused]] const auto scratchTextureRead =
                builder.Read(readerTextureHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Writer", "Reader");
    graph.SetFinalPass("Reader");
    graph.BuildFrameGraph();

    ASSERT_TRUE(importedSceneColor.IsValid());
    ASSERT_TRUE(importedSceneDepth.IsValid());
    EXPECT_EQ(std::string(graph.GetResourceName(importedSceneColor)), std::string(ResourceNames::SceneColor));
    EXPECT_EQ(std::string(graph.GetResourceName(importedSceneDepth)), std::string(ResourceNames::SceneDepth));

    const auto postBuildSceneColor = graph.GetFramebufferHandle(ResourceNames::SceneColor);
    const auto postBuildSceneDepth = graph.GetTextureHandle(ResourceNames::SceneDepth);
    EXPECT_EQ(importedSceneColor.Index, postBuildSceneColor.Index);
    EXPECT_EQ(importedSceneColor.Generation, postBuildSceneColor.Generation);
    EXPECT_EQ(importedSceneDepth.Index, postBuildSceneDepth.Index);
    EXPECT_EQ(importedSceneDepth.Generation, postBuildSceneDepth.Generation);

    ASSERT_TRUE(writerFramebufferHandle.IsValid());
    ASSERT_TRUE(readerFramebufferHandle.IsValid());
    EXPECT_EQ(writerFramebufferHandle.Index, readerFramebufferHandle.Index);
    EXPECT_EQ(writerFramebufferHandle.Generation, readerFramebufferHandle.Generation);

    ASSERT_TRUE(writerTextureHandle.IsValid());
    ASSERT_TRUE(readerTextureHandle.IsValid());
    EXPECT_EQ(writerTextureHandle.Index, readerTextureHandle.Index);
    EXPECT_EQ(writerTextureHandle.Generation, readerTextureHandle.Generation);

    const auto namedFramebufferHandle = graph.GetFramebufferHandle("StableScratchFB");
    ASSERT_TRUE(namedFramebufferHandle.IsValid());
    EXPECT_EQ(writerFramebufferHandle.Index, namedFramebufferHandle.Index);
    EXPECT_EQ(writerFramebufferHandle.Generation, namedFramebufferHandle.Generation);

    const auto namedTextureHandle = graph.GetTextureHandle("StableScratchTex");
    ASSERT_TRUE(namedTextureHandle.IsValid());
    EXPECT_EQ(writerTextureHandle.Index, namedTextureHandle.Index);
    EXPECT_EQ(writerTextureHandle.Generation, namedTextureHandle.Generation);
}

TEST(RenderGraphTypedHandles, FramebufferAttachmentViewResolvesImportedFramebufferAttachment)
{
    RenderGraph graph;

    FramebufferSpecification spec;
    spec.Width = 640u;
    spec.Height = 360u;
    spec.Attachments = FramebufferAttachmentSpecification{
        FramebufferTextureSpecification{ FramebufferTextureFormat::RGBA16F },
        FramebufferTextureSpecification{ FramebufferTextureFormat::RG16F },
        FramebufferTextureSpecification{ FramebufferTextureFormat::DEPTH24STENCIL8 }
    };

    Ref<MultiAttachmentStubFramebuffer> framebuffer = Ref<MultiAttachmentStubFramebuffer>::Create(77u, std::initializer_list<u32>{ 101u, 202u });
    framebuffer->Resize(spec.Width, spec.Height);

    RGResourceDesc oitDesc;
    oitDesc.Kind = ResourceHandle::Kind::Framebuffer;
    oitDesc.Width = spec.Width;
    oitDesc.Height = spec.Height;
    oitDesc.Attachments = {
        RGResourceFormat::RGBA16Float,
        RGResourceFormat::RG16Float,
        RGResourceFormat::Depth24Stencil8,
    };
    oitDesc.DebugName = std::string(ResourceNames::OITBuffer);

    const auto oitHandle = graph.ImportFramebuffer(ResourceNames::OITBuffer, framebuffer, oitDesc);
    const auto accumView = graph.CreateFramebufferAttachmentView(ResourceNames::OITAccum, oitHandle, 0u);
    const auto revealageView = graph.CreateFramebufferAttachmentView(ResourceNames::OITRevealage, oitHandle, 1u);

    ASSERT_TRUE(accumView.IsValid());
    ASSERT_TRUE(revealageView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(accumView), 101u);
    EXPECT_EQ(graph.ResolveTexture(revealageView), 202u);

    const auto* accumInfo = graph.FindRegisteredResource(ResourceNames::OITAccum);
    ASSERT_NE(accumInfo, nullptr);
    EXPECT_EQ(accumInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(accumInfo->Desc.Format, RGResourceFormat::RGBA16Float);

    const auto* revealageInfo = graph.FindRegisteredResource(ResourceNames::OITRevealage);
    ASSERT_NE(revealageInfo, nullptr);
    EXPECT_EQ(revealageInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(revealageInfo->Desc.Format, RGResourceFormat::RG16Float);
}

TEST(RenderGraphTypedHandles, FramebufferDepthAttachmentViewResolvesImportedFramebufferDepthAttachment)
{
    RenderGraph graph;

    auto framebuffer = Ref<MultiAttachmentStubFramebuffer>::Create(91u, std::initializer_list<u32>{ 111u }, 303u);

    RGResourceDesc depthDesc;
    depthDesc.Kind = ResourceHandle::Kind::Framebuffer;
    depthDesc.Width = 800u;
    depthDesc.Height = 600u;
    depthDesc.Samples = 4u;
    depthDesc.Attachments = {
        RGResourceFormat::RGBA16Float,
        RGResourceFormat::Depth24Stencil8,
    };
    depthDesc.DebugName = "DepthViewParent";

    const auto framebufferHandle = graph.ImportFramebuffer("DepthViewParent", framebuffer, depthDesc);
    const auto depthView = graph.CreateFramebufferDepthAttachmentView("DepthView", framebufferHandle);

    ASSERT_TRUE(depthView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(depthView), 303u);

    const auto* depthInfo = graph.FindRegisteredResource("DepthView");
    ASSERT_NE(depthInfo, nullptr);
    EXPECT_EQ(depthInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(depthInfo->Desc.Format, RGResourceFormat::Depth24Stencil8);
    EXPECT_EQ(depthInfo->Desc.Samples, 4u);
}

TEST(RenderGraphTypedHandles, ExternallyBackedTransientFramebufferViewsResolveBackingAndRemainTransient)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto framebuffer = Ref<MultiAttachmentStubFramebuffer>::Create(1337u, std::initializer_list<u32>{ 101u, 202u }, 303u);
    framebuffer->Resize(640u, 360u);

    RGResourceDesc gbufferDesc;
    gbufferDesc.Kind = ResourceHandle::Kind::Framebuffer;
    gbufferDesc.Width = 640u;
    gbufferDesc.Height = 360u;
    gbufferDesc.Attachments = {
        RGResourceFormat::RGBA8UNorm,
        RGResourceFormat::RGBA16Float,
        RGResourceFormat::Depth24Stencil8,
    };
    gbufferDesc.DebugName = "ExternallyBackedGBuffer";

    const auto gbufferHandle = graph.DeclareTransientFramebuffer("ExternallyBackedGBuffer", gbufferDesc, framebuffer);
    const auto normalView = graph.CreateFramebufferAttachmentView("ExternallyBackedGBufferNormal", gbufferHandle, 1u);
    const auto depthView = graph.CreateFramebufferDepthAttachmentView("ExternallyBackedGBufferDepth", gbufferHandle);

    ASSERT_TRUE(gbufferHandle.IsValid());
    ASSERT_TRUE(normalView.IsValid());
    ASSERT_TRUE(depthView.IsValid());
    EXPECT_EQ(graph.ResolveFramebuffer(gbufferHandle), framebuffer);
    EXPECT_EQ(graph.ResolveTexture(normalView), 202u);
    EXPECT_EQ(graph.ResolveTexture(depthView), 303u);

    AddSetupNode(graph, "Writer", [gbufferHandle](RGBuilder& builder)
                 { builder.Write(gbufferHandle, RGWriteUsage::RenderTarget); });
    AddSetupNode(graph, "Reader", [normalView, depthView](RGBuilder& builder)
                 {
                     [[maybe_unused]] const auto normalRead = builder.Read(normalView, RGReadUsage::ShaderSample);
                     [[maybe_unused]] const auto depthRead = builder.Read(depthView, RGReadUsage::ShaderSample); });

    graph.AddExecutionDependency("Writer", "Reader");
    graph.SetFinalPass("Reader");
    graph.BuildFrameGraph();

    const auto* gbufferInfo = graph.FindRegisteredResource("ExternallyBackedGBuffer");
    ASSERT_NE(gbufferInfo, nullptr);
    EXPECT_EQ(gbufferInfo->Desc.Kind, ResourceHandle::Kind::Framebuffer);
    EXPECT_FALSE(gbufferInfo->Desc.Imported);
    EXPECT_TRUE(gbufferInfo->HasExternalBacking);

    const auto* normalInfo = graph.FindRegisteredResource("ExternallyBackedGBufferNormal");
    ASSERT_NE(normalInfo, nullptr);
    EXPECT_TRUE(normalInfo->HasExternalBacking);

    const auto* depthInfo = graph.FindRegisteredResource("ExternallyBackedGBufferDepth");
    ASSERT_NE(depthInfo, nullptr);
    EXPECT_TRUE(depthInfo->HasExternalBacking);

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::ranges::find_if(transientPlan,
                                             [](const RenderGraph::TransientPlanEntry& entry)
                                             {
                                                 return entry.Resource == "ExternallyBackedGBuffer";
                                             });
    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "external-backing");

    const auto lifetimes = graph.GetResourceLifetimes();
    const auto lifetimeIt = std::ranges::find_if(lifetimes,
                                                 [](const RenderGraph::ResourceLifetime& lifetime)
                                                 {
                                                     return lifetime.ResourceName == "ExternallyBackedGBuffer";
                                                 });
    ASSERT_NE(lifetimeIt, lifetimes.end());
    EXPECT_FALSE(lifetimeIt->IsImported);
    EXPECT_TRUE(lifetimeIt->IsTransient);

    const auto normalLifetimeIt = std::ranges::find_if(lifetimes,
                                                       [](const RenderGraph::ResourceLifetime& lifetime)
                                                       {
                                                           return lifetime.ResourceName == "ExternallyBackedGBufferNormal";
                                                       });
    ASSERT_NE(normalLifetimeIt, lifetimes.end());
    EXPECT_FALSE(normalLifetimeIt->IsImported);
    EXPECT_TRUE(normalLifetimeIt->IsTransient);
    EXPECT_TRUE(normalLifetimeIt->HasExternalBacking);

    const auto depthLifetimeIt = std::ranges::find_if(lifetimes,
                                                      [](const RenderGraph::ResourceLifetime& lifetime)
                                                      {
                                                          return lifetime.ResourceName == "ExternallyBackedGBufferDepth";
                                                      });
    ASSERT_NE(depthLifetimeIt, lifetimes.end());
    EXPECT_FALSE(depthLifetimeIt->IsImported);
    EXPECT_TRUE(depthLifetimeIt->IsTransient);
    EXPECT_TRUE(depthLifetimeIt->HasExternalBacking);

    EXPECT_TRUE(lifetimeIt->HasExternalBacking);

    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_external_backing_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"externallyBackedTransientRootCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"externallyBackedResourceCount\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"hasExternalBacking\": true"), std::string::npos);
    EXPECT_NE(json.find("\"resource\": \"ExternallyBackedGBufferNormal\", \"isImported\": false, \"isExtracted\": false, \"isHistory\": false, \"isTransient\": true, \"hasExternalBacking\": true"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraphTypedHandles, ExternallyBackedTransientTextureViewsResolveBackingAndRemainTransient)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto csmDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, "ExternallyBackedShadowCSM");
    csmDesc.Format = RGResourceFormat::Depth32Float;
    csmDesc.Width = 2048u;
    csmDesc.Height = 2048u;
    csmDesc.DepthOrLayers = 4u;

    auto pointDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, "ExternallyBackedShadowPoint");
    pointDesc.Format = RGResourceFormat::Depth32Float;
    pointDesc.Width = 1024u;
    pointDesc.Height = 1024u;
    pointDesc.DepthOrLayers = 6u;

    const auto csmHandle = graph.DeclareTransientTexture("ExternallyBackedShadowCSM", csmDesc, 1401u);
    const auto cascadeView = graph.CreateTextureArrayLayerView("ExternallyBackedShadowCSMCascade2", csmHandle, 2u);
    const auto pointHandle = graph.DeclareTransientTexture("ExternallyBackedShadowPoint", pointDesc, 1402u);
    const auto faceView = graph.CreateTextureCubeFaceView("ExternallyBackedShadowPointFace4", pointHandle, 4u);

    ASSERT_TRUE(csmHandle.IsValid());
    ASSERT_TRUE(cascadeView.IsValid());
    ASSERT_TRUE(pointHandle.IsValid());
    ASSERT_TRUE(faceView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(csmHandle), 1401u);
    EXPECT_EQ(graph.ResolveTexture(cascadeView), 1401u);
    EXPECT_EQ(graph.ResolveTexture(pointHandle), 1402u);
    EXPECT_EQ(graph.ResolveTexture(faceView), 1402u);

    AddSetupNode(graph, "ShadowWriter", [csmHandle, pointHandle](RGBuilder& builder)
                 {
                     builder.Write(csmHandle, RGWriteUsage::DepthStencil);
                     builder.Write(pointHandle, RGWriteUsage::DepthStencil); });
    AddSetupNode(graph, "ShadowReader", [cascadeView, faceView](RGBuilder& builder)
                 {
                     [[maybe_unused]] const auto cascadeRead = builder.Read(cascadeView, RGReadUsage::ShaderSample);
                     [[maybe_unused]] const auto faceRead = builder.Read(faceView, RGReadUsage::ShaderSample); });

    graph.AddExecutionDependency("ShadowWriter", "ShadowReader");
    graph.SetFinalPass("ShadowReader");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());

    const auto* csmInfo = graph.FindRegisteredResource("ExternallyBackedShadowCSM");
    ASSERT_NE(csmInfo, nullptr);
    EXPECT_FALSE(csmInfo->Desc.Imported);
    EXPECT_TRUE(csmInfo->HasExternalBacking);

    const auto* cascadeInfo = graph.FindRegisteredResource("ExternallyBackedShadowCSMCascade2");
    ASSERT_NE(cascadeInfo, nullptr);
    EXPECT_FALSE(cascadeInfo->Desc.Imported);
    EXPECT_TRUE(cascadeInfo->HasExternalBacking);

    const auto* pointInfo = graph.FindRegisteredResource("ExternallyBackedShadowPoint");
    ASSERT_NE(pointInfo, nullptr);
    EXPECT_FALSE(pointInfo->Desc.Imported);
    EXPECT_TRUE(pointInfo->HasExternalBacking);

    const auto* faceInfo = graph.FindRegisteredResource("ExternallyBackedShadowPointFace4");
    ASSERT_NE(faceInfo, nullptr);
    EXPECT_FALSE(faceInfo->Desc.Imported);
    EXPECT_TRUE(faceInfo->HasExternalBacking);

    const auto& transientPlan = graph.GetTransientPlan();
    const auto csmPlanIt = std::ranges::find_if(transientPlan,
                                                [](const RenderGraph::TransientPlanEntry& entry)
                                                {
                                                    return entry.Resource == "ExternallyBackedShadowCSM";
                                                });
    ASSERT_NE(csmPlanIt, transientPlan.end());
    EXPECT_TRUE(csmPlanIt->Reachable);
    EXPECT_FALSE(csmPlanIt->WillAllocate);
    EXPECT_EQ(csmPlanIt->SkipReason, "external-backing");

    const auto pointPlanIt = std::ranges::find_if(transientPlan,
                                                  [](const RenderGraph::TransientPlanEntry& entry)
                                                  {
                                                      return entry.Resource == "ExternallyBackedShadowPoint";
                                                  });
    ASSERT_NE(pointPlanIt, transientPlan.end());
    EXPECT_TRUE(pointPlanIt->Reachable);
    EXPECT_FALSE(pointPlanIt->WillAllocate);
    EXPECT_EQ(pointPlanIt->SkipReason, "external-backing");

    const auto lifetimes = graph.GetResourceLifetimes();
    const auto expectLifetime = [&lifetimes](std::string_view resourceName)
    {
        const auto lifetimeIt = std::ranges::find_if(lifetimes,
                                                     [resourceName](const RenderGraph::ResourceLifetime& lifetime)
                                                     {
                                                         return lifetime.ResourceName == resourceName;
                                                     });
        ASSERT_NE(lifetimeIt, lifetimes.end());
        EXPECT_FALSE(lifetimeIt->IsImported);
        EXPECT_TRUE(lifetimeIt->IsTransient);
        EXPECT_TRUE(lifetimeIt->HasExternalBacking);
    };

    expectLifetime("ExternallyBackedShadowCSM");
    expectLifetime("ExternallyBackedShadowCSMCascade2");
    expectLifetime("ExternallyBackedShadowPoint");
    expectLifetime("ExternallyBackedShadowPointFace4");

    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_external_texture_backing_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"externallyBackedTransientRootCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"externallyBackedResourceCount\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"resource\": \"ExternallyBackedShadowCSMCascade2\", \"isImported\": false, \"isExtracted\": false, \"isHistory\": false, \"isTransient\": true, \"hasExternalBacking\": true"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraphTypedHandles, FramebufferAttachmentViewsCanBeCreatedFromTransientFramebufferDescriptors)
{
    RenderGraph graph;

    RGResourceDesc sceneDesc;
    sceneDesc.Kind = ResourceHandle::Kind::Framebuffer;
    sceneDesc.Width = 1280u;
    sceneDesc.Height = 720u;
    sceneDesc.Attachments = {
        RGResourceFormat::RGBA16Float,
        RGResourceFormat::R32Int,
        RGResourceFormat::RG16Float,
        RGResourceFormat::RG16Float,
        RGResourceFormat::Depth24Stencil8,
    };
    sceneDesc.DebugName = std::string(ResourceNames::SceneColor);

    const auto sceneHandle = graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, sceneDesc);
    const auto sceneColorTexture = graph.CreateFramebufferAttachmentView(ResourceNames::SceneColorTexture, sceneHandle, 0u);
    const auto sceneEntityID = graph.CreateFramebufferAttachmentView(ResourceNames::SceneEntityID, sceneHandle, 1u);
    const auto sceneViewNormals = graph.CreateFramebufferAttachmentView(ResourceNames::SceneViewNormals, sceneHandle, 2u);
    const auto sceneDepthAttachment = graph.CreateFramebufferDepthAttachmentView(ResourceNames::SceneDepthAttachment, sceneHandle);

    ASSERT_TRUE(sceneColorTexture.IsValid());
    ASSERT_TRUE(sceneEntityID.IsValid());
    ASSERT_TRUE(sceneViewNormals.IsValid());
    ASSERT_TRUE(sceneDepthAttachment.IsValid());

    const auto* sceneColorInfo = graph.FindRegisteredResource(ResourceNames::SceneColorTexture);
    ASSERT_NE(sceneColorInfo, nullptr);
    EXPECT_EQ(sceneColorInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(sceneColorInfo->Desc.Format, RGResourceFormat::RGBA16Float);

    const auto* entityInfo = graph.FindRegisteredResource(ResourceNames::SceneEntityID);
    ASSERT_NE(entityInfo, nullptr);
    EXPECT_EQ(entityInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(entityInfo->Desc.Format, RGResourceFormat::R32Int);

    const auto* normalsInfo = graph.FindRegisteredResource(ResourceNames::SceneViewNormals);
    ASSERT_NE(normalsInfo, nullptr);
    EXPECT_EQ(normalsInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(normalsInfo->Desc.Format, RGResourceFormat::RG16Float);

    const auto* depthInfo = graph.FindRegisteredResource(ResourceNames::SceneDepthAttachment);
    ASSERT_NE(depthInfo, nullptr);
    EXPECT_EQ(depthInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(depthInfo->Desc.Format, RGResourceFormat::Depth24Stencil8);
}

TEST(RenderGraphTypedHandles, TextureMipViewResolvesImportedTextureAndTracksMipDimensions)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "HZBDepthParent");
    textureDesc.Format = RGResourceFormat::R32Float;
    textureDesc.Width = 512u;
    textureDesc.Height = 256u;
    textureDesc.MipLevels = 6u;

    const auto textureHandle = graph.ImportTexture("HZBDepthParent", 707u, textureDesc);
    const auto mipView = graph.CreateTextureMipView("HZBDepthParentMip3", textureHandle, 3u);

    ASSERT_TRUE(textureHandle.IsValid());
    ASSERT_TRUE(mipView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(mipView), 707u);

    const auto* mipInfo = graph.FindRegisteredResource("HZBDepthParentMip3");
    ASSERT_NE(mipInfo, nullptr);
    EXPECT_EQ(mipInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(mipInfo->Desc.Format, RGResourceFormat::R32Float);
    EXPECT_EQ(mipInfo->Desc.Width, 64u);
    EXPECT_EQ(mipInfo->Desc.Height, 32u);
    EXPECT_EQ(mipInfo->Desc.MipLevels, 1u);
}

TEST(RenderGraphTypedHandles, TextureMipViewsCanBeCreatedFromTransientTextureDescriptors)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::HZBDepth));
    textureDesc.Format = RGResourceFormat::R32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 512u;
    textureDesc.MipLevels = 11u;

    const auto textureHandle = graph.AllocateTransientTextureHandle(ResourceNames::HZBDepth, textureDesc);
    const auto mipView = graph.CreateTextureMipView("HZBDepthMip5", textureHandle, 5u);

    ASSERT_TRUE(textureHandle.IsValid());
    ASSERT_TRUE(mipView.IsValid());

    const auto* mipInfo = graph.FindRegisteredResource("HZBDepthMip5");
    ASSERT_NE(mipInfo, nullptr);
    EXPECT_EQ(mipInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(mipInfo->Desc.Format, RGResourceFormat::R32Float);
    EXPECT_EQ(mipInfo->Desc.Width, 32u);
    EXPECT_EQ(mipInfo->Desc.Height, 16u);
    EXPECT_EQ(mipInfo->Desc.MipLevels, 1u);
}

TEST(RenderGraphTypedHandles, TextureArrayLayerViewResolvesImportedTextureArrayAndTracksLayerMetadata)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, "ShadowCSMParent");
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 2048u;
    textureDesc.Height = 2048u;
    textureDesc.DepthOrLayers = 4u;

    const auto textureHandle = graph.ImportTexture("ShadowCSMParent", 719u, textureDesc);
    const auto layerView = graph.CreateTextureArrayLayerView("ShadowCSMParentCascade2", textureHandle, 2u);

    ASSERT_TRUE(textureHandle.IsValid());
    ASSERT_TRUE(layerView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(layerView), 719u);

    const auto* layerInfo = graph.FindRegisteredResource("ShadowCSMParentCascade2");
    ASSERT_NE(layerInfo, nullptr);
    EXPECT_EQ(layerInfo->Desc.Kind, ResourceHandle::Kind::Texture2DArray);
    EXPECT_EQ(layerInfo->Desc.Format, RGResourceFormat::Depth32Float);
    EXPECT_EQ(layerInfo->Desc.Width, 2048u);
    EXPECT_EQ(layerInfo->Desc.Height, 2048u);
    EXPECT_EQ(layerInfo->Desc.DepthOrLayers, 1u);
}

TEST(RenderGraphTypedHandles, TextureArrayLayerViewsCanBeCreatedFromTransientTextureDescriptors)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, std::string(ResourceNames::ShadowMapCSM));
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 1024u;
    textureDesc.DepthOrLayers = 4u;

    const auto textureHandle = graph.AllocateTransientTextureHandle(ResourceNames::ShadowMapCSM, textureDesc);
    const auto layerView = graph.CreateTextureArrayLayerView(ResourceNames::ShadowMapCSMCascade2, textureHandle, 2u);

    ASSERT_TRUE(textureHandle.IsValid());
    ASSERT_TRUE(layerView.IsValid());

    const auto* layerInfo = graph.FindRegisteredResource(ResourceNames::ShadowMapCSMCascade2);
    ASSERT_NE(layerInfo, nullptr);
    EXPECT_EQ(layerInfo->Desc.Kind, ResourceHandle::Kind::Texture2DArray);
    EXPECT_EQ(layerInfo->Desc.Format, RGResourceFormat::Depth32Float);
    EXPECT_EQ(layerInfo->Desc.Width, 1024u);
    EXPECT_EQ(layerInfo->Desc.Height, 1024u);
    EXPECT_EQ(layerInfo->Desc.DepthOrLayers, 1u);
}

TEST(RenderGraphTypedHandles, TextureCubeFaceViewResolvesImportedCubeTextureAndTracksFaceMetadata)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, "ShadowPointParent");
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 1024u;
    textureDesc.DepthOrLayers = 6u;

    const auto textureHandle = graph.ImportTexture("ShadowPointParent", 727u, textureDesc);
    const auto faceView = graph.CreateTextureCubeFaceView("ShadowPointParentFace4", textureHandle, 4u);

    ASSERT_TRUE(textureHandle.IsValid());
    ASSERT_TRUE(faceView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(faceView), 727u);

    const auto* faceInfo = graph.FindRegisteredResource("ShadowPointParentFace4");
    ASSERT_NE(faceInfo, nullptr);
    EXPECT_EQ(faceInfo->Desc.Kind, ResourceHandle::Kind::TextureCube);
    EXPECT_EQ(faceInfo->Desc.Format, RGResourceFormat::Depth32Float);
    EXPECT_EQ(faceInfo->Desc.Width, 1024u);
    EXPECT_EQ(faceInfo->Desc.Height, 1024u);
    EXPECT_EQ(faceInfo->Desc.DepthOrLayers, 1u);
}

TEST(RenderGraphTypedHandles, TextureMultisampleResolveViewResolvesSingleSampleBackingAndTracksResolvedMetadata)
{
    RenderGraph graph;

    auto multisampleDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "GBufferAlbedoMSParent");
    multisampleDesc.Format = RGResourceFormat::RGBA8UNorm;
    multisampleDesc.Width = 1280u;
    multisampleDesc.Height = 720u;
    multisampleDesc.Samples = 4u;

    auto resolvedDesc = multisampleDesc;
    resolvedDesc.DebugName = "GBufferAlbedoResolvedBacking";
    resolvedDesc.Samples = 1u;

    const auto multisampleHandle = graph.ImportTexture("GBufferAlbedoMSParent", 751u, multisampleDesc);
    const auto resolvedHandle = graph.ImportTexture("GBufferAlbedoResolvedBacking", 752u, resolvedDesc);
    const auto resolveView =
        graph.CreateTextureMultisampleResolveView("GBufferAlbedoResolved", multisampleHandle, resolvedHandle);

    ASSERT_TRUE(multisampleHandle.IsValid());
    ASSERT_TRUE(resolvedHandle.IsValid());
    ASSERT_TRUE(resolveView.IsValid());
    EXPECT_EQ(graph.ResolveTexture(resolveView), 752u);

    const auto* resolveInfo = graph.FindRegisteredResource("GBufferAlbedoResolved");
    ASSERT_NE(resolveInfo, nullptr);
    EXPECT_EQ(resolveInfo->Desc.Kind, ResourceHandle::Kind::Texture2D);
    EXPECT_EQ(resolveInfo->Desc.Format, RGResourceFormat::RGBA8UNorm);
    EXPECT_EQ(resolveInfo->Desc.Width, 1280u);
    EXPECT_EQ(resolveInfo->Desc.Height, 720u);
    EXPECT_EQ(resolveInfo->Desc.Samples, 1u);
}

TEST(RenderGraphTypedHandles, MultisampleParentWriterFeedsResolveViewReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto multisampleDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AliasedResolveTextureMS");
    multisampleDesc.Format = RGResourceFormat::RGBA16Float;
    multisampleDesc.Width = 1280u;
    multisampleDesc.Height = 720u;
    multisampleDesc.Samples = 4u;

    auto resolvedDesc = multisampleDesc;
    resolvedDesc.DebugName = "AliasedResolveTextureBacking";
    resolvedDesc.Samples = 1u;

    const auto multisampleHandle = graph.ImportTexture("AliasedResolveTextureMS", 761u, multisampleDesc);
    const auto resolvedHandle = graph.ImportTexture("AliasedResolveTextureBacking", 762u, resolvedDesc);
    const auto resolveView =
        graph.CreateTextureMultisampleResolveView("AliasedResolveTexture", multisampleHandle, resolvedHandle);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("ResolveSourceWriterPass");
    writerPass->SetSetupBehavior(
        [multisampleHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(multisampleHandle, RGWriteUsage::TransferDest);
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("ResolveViewReaderPass");
    readerPass->SetSetupBehavior(
        [resolveView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto resolvedRead = builder.Read(resolveView, RGReadUsage::ShaderSample);
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("ResolveViewReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "ResolveSourceWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "ResolveViewReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());

    const auto transitions = graph.GetResourceTransitions();
    const auto transitionIt = std::ranges::find_if(transitions,
                                                   [](const RenderGraph::ResourceTransition& transition)
                                                   {
                                                       return transition.ResourceName == "AliasedResolveTexture" &&
                                                              transition.ProducerPass == "ResolveSourceWriterPass" &&
                                                              transition.ConsumerPass == "ResolveViewReaderPass";
                                                   });
    EXPECT_NE(transitionIt, transitions.end());
}

TEST(RenderGraphTypedHandles, ParentFramebufferWriterFeedsAttachmentViewReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto framebuffer = Ref<AttachmentStubFramebuffer>::Create(501u, 701u);

    auto framebufferDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "AliasedFramebuffer");
    framebufferDesc.Format = RGResourceFormat::RGBA16Float;
    framebufferDesc.Width = 1u;
    framebufferDesc.Height = 1u;

    const auto framebufferHandle = graph.ImportFramebuffer("AliasedFramebuffer", framebuffer, framebufferDesc);
    const auto attachmentView = graph.CreateFramebufferAttachmentView("AliasedFramebufferTexture", framebufferHandle, 0u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("FramebufferWriterPass");
    writerPass->SetSetupBehavior(
        [framebufferHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(framebufferHandle, RGWriteUsage::RenderTarget);
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("AttachmentViewReaderPass");
    readerPass->SetSetupBehavior(
        [attachmentView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto readHandle = builder.Read(attachmentView, RGReadUsage::ShaderSample);
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("AttachmentViewReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "FramebufferWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "AttachmentViewReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());

    const auto& culledPasses = graph.GetCulledPasses();
    EXPECT_TRUE(std::ranges::find(culledPasses, "FramebufferWriterPass") == culledPasses.end());

    const auto transitions = graph.GetResourceTransitions();
    const auto transitionIt = std::ranges::find_if(transitions,
                                                   [](const RenderGraph::ResourceTransition& transition)
                                                   {
                                                       return transition.ResourceName == "AliasedFramebufferTexture" &&
                                                              transition.ProducerPass == "FramebufferWriterPass" &&
                                                              transition.ConsumerPass == "AttachmentViewReaderPass";
                                                   });
    EXPECT_NE(transitionIt, transitions.end());
}

TEST(RenderGraphTypedHandles, ParentTextureWriterFeedsMipViewReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AliasedMipTexture");
    textureDesc.Format = RGResourceFormat::R32Float;
    textureDesc.Width = 512u;
    textureDesc.Height = 256u;
    textureDesc.MipLevels = 6u;

    const auto textureHandle = graph.ImportTexture("AliasedMipTexture", 911u, textureDesc);
    const auto mipView = graph.CreateTextureMipView("AliasedMipTextureMip2", textureHandle, 2u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("TextureWriterPass");
    writerPass->SetSetupBehavior(
        [textureHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(textureHandle, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(2u));
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("MipViewReaderPass");
    readerPass->SetSetupBehavior(
        [mipView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto mipRead = builder.Read(mipView, RGReadUsage::ShaderSample);
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("MipViewReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "TextureWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "MipViewReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());
}

TEST(RenderGraphTypedHandles, MipViewWriterFeedsParentTextureReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AliasedMipTexture");
    textureDesc.Format = RGResourceFormat::R32Float;
    textureDesc.Width = 512u;
    textureDesc.Height = 256u;
    textureDesc.MipLevels = 6u;

    const auto textureHandle = graph.ImportTexture("AliasedMipTexture", 913u, textureDesc);
    const auto mipView = graph.CreateTextureMipView("AliasedMipTextureMip2", textureHandle, 2u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("MipViewWriterPass");
    writerPass->SetSetupBehavior(
        [mipView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(mipView, RGWriteUsage::ShaderImage);
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("TextureReaderPass");
    readerPass->SetSetupBehavior(
        [textureHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto mipRead = builder.Read(textureHandle, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(2u));
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("TextureReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "MipViewWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "TextureReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());
}

TEST(RenderGraphTypedHandles, ParentTextureWriterFeedsArrayLayerViewReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, "AliasedLayerTexture");
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 1024u;
    textureDesc.DepthOrLayers = 4u;

    const auto textureHandle = graph.ImportTexture("AliasedLayerTexture", 931u, textureDesc);
    const auto layerView = graph.CreateTextureArrayLayerView("AliasedLayerTextureLayer2", textureHandle, 2u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("LayerTextureWriterPass");
    writerPass->SetSetupBehavior(
        [textureHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(textureHandle, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(2u));
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("LayerViewReaderPass");
    readerPass->SetSetupBehavior(
        [layerView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto layerRead = builder.Read(layerView, RGReadUsage::ShaderSample);
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("LayerViewReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "LayerTextureWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "LayerViewReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());
}

TEST(RenderGraphTypedHandles, ArrayLayerViewWriterFeedsParentTextureReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, "AliasedLayerTexture");
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 1024u;
    textureDesc.DepthOrLayers = 4u;

    const auto textureHandle = graph.ImportTexture("AliasedLayerTexture", 937u, textureDesc);
    const auto layerView = graph.CreateTextureArrayLayerView("AliasedLayerTextureLayer2", textureHandle, 2u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("LayerViewWriterPass");
    writerPass->SetSetupBehavior(
        [layerView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(layerView, RGWriteUsage::DepthStencil);
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("LayerTextureReaderPass");
    readerPass->SetSetupBehavior(
        [textureHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto layerRead =
                builder.Read(textureHandle, RGReadUsage::ShaderSample, RGSubresourceRange::Layer(2u));
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("LayerTextureReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "LayerViewWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "LayerTextureReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());
}

TEST(RenderGraphTypedHandles, ParentTextureWriterFeedsCubeFaceViewReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, "AliasedCubeTexture");
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 1024u;
    textureDesc.DepthOrLayers = 6u;

    const auto textureHandle = graph.ImportTexture("AliasedCubeTexture", 941u, textureDesc);
    const auto faceView = graph.CreateTextureCubeFaceView("AliasedCubeTextureFace4", textureHandle, 4u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("CubeTextureWriterPass");
    writerPass->SetSetupBehavior(
        [textureHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            RGSubresourceRange faceRange{};
            faceRange.BaseSlice = 4u;
            faceRange.SliceCount = 1u;
            builder.Write(textureHandle, RGWriteUsage::DepthStencil, faceRange);
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("CubeFaceViewReaderPass");
    readerPass->SetSetupBehavior(
        [faceView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            [[maybe_unused]] const auto faceRead = builder.Read(faceView, RGReadUsage::ShaderSample);
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("CubeFaceViewReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "CubeTextureWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "CubeFaceViewReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());
}

TEST(RenderGraphTypedHandles, CubeFaceViewWriterFeedsParentTextureReaderAcrossCompileStages)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto textureDesc = RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, "AliasedCubeTexture");
    textureDesc.Format = RGResourceFormat::Depth32Float;
    textureDesc.Width = 1024u;
    textureDesc.Height = 1024u;
    textureDesc.DepthOrLayers = 6u;

    const auto textureHandle = graph.ImportTexture("AliasedCubeTexture", 947u, textureDesc);
    const auto faceView = graph.CreateTextureCubeFaceView("AliasedCubeTextureFace4", textureHandle, 4u);

    auto writerPass = Ref<CallbackStyleStubPass>::Create("CubeFaceViewWriterPass");
    writerPass->SetSetupBehavior(
        [faceView](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            builder.Write(faceView, RGWriteUsage::DepthStencil);
        });
    AddPassNode(graph, writerPass);

    auto readerPass = Ref<CallbackStyleStubPass>::Create("CubeTextureReaderPass");
    readerPass->SetSetupBehavior(
        [textureHandle](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
        {
            RGSubresourceRange faceRange{};
            faceRange.BaseSlice = 4u;
            faceRange.SliceCount = 1u;
            [[maybe_unused]] const auto faceRead = builder.Read(textureHandle, RGReadUsage::ShaderSample, faceRange);
        });
    AddPassNode(graph, readerPass);

    graph.SetFinalPass("CubeTextureReaderPass");
    graph.BuildFrameGraph();

    const auto& executionOrder = graph.GetExecutionOrder();
    const auto writerIt = std::ranges::find(executionOrder, "CubeFaceViewWriterPass");
    const auto readerIt = std::ranges::find(executionOrder, "CubeTextureReaderPass");
    ASSERT_NE(writerIt, executionOrder.end());
    ASSERT_NE(readerIt, executionOrder.end());
    EXPECT_LT(writerIt, readerIt);

    const auto hazards = graph.ValidateCompiledResourceHazards();
    EXPECT_TRUE(hazards.empty());
}

// Verify that the graph-owned OIT descriptor remains incompatible with the old
// color-only MRT shape so alias planning cannot silently regress to a bridge
// era descriptor that omits depth.
TEST(RenderGraphTransientPool, PhaseD_OITBufferDepthAttachmentParticipatesInCompatibility)
{
    RGResourceDesc colorOnly;
    colorOnly.Kind = ResourceHandle::Kind::Framebuffer;
    colorOnly.Width = 1280;
    colorOnly.Height = 720;
    colorOnly.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float };

    RGResourceDesc withDepth;
    withDepth.Kind = ResourceHandle::Kind::Framebuffer;
    withDepth.Width = 1280;
    withDepth.Height = 720;
    withDepth.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float, RGResourceFormat::Depth24Stencil8 };

    EXPECT_FALSE(withDepth.IsCompatibleWith(colorOnly));
    EXPECT_FALSE(colorOnly.IsCompatibleWith(withDepth));
}

// Verify that MRT alias group keys differ from single-attachment keys so
// they are never incorrectly aliased with single-format framebuffers.
TEST(RenderGraphTransientPool, PhaseD_MRTAliasGroupDiffersFromSingleAttachmentKey)
{
    RGResourceDesc single;
    single.Kind = ResourceHandle::Kind::Framebuffer;
    single.Format = RGResourceFormat::RGBA16Float;
    single.Width = 1280;
    single.Height = 720;

    RGResourceDesc mrt;
    mrt.Kind = ResourceHandle::Kind::Framebuffer;
    mrt.Width = 1280;
    mrt.Height = 720;
    mrt.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float, RGResourceFormat::Depth24Stencil8 };

    // Use the IsCompatibleWith predicate: MRT and single must NOT be compatible.
    EXPECT_FALSE(mrt.IsCompatibleWith(single))
        << "MRT desc must not be compatible with a single-attachment desc";
    EXPECT_FALSE(single.IsCompatibleWith(mrt))
        << "Single-attachment desc must not be compatible with an MRT desc";

    // MRT descriptor with different attachment order must also differ.
    RGResourceDesc mrt2;
    mrt2.Kind = ResourceHandle::Kind::Framebuffer;
    mrt2.Width = 1280;
    mrt2.Height = 720;
    mrt2.Attachments = { RGResourceFormat::RG16Float, RGResourceFormat::RGBA16Float, RGResourceFormat::Depth24Stencil8 };

    EXPECT_FALSE(mrt.IsCompatibleWith(mrt2))
        << "MRT descs with different attachment orders must not be compatible";
}

// Verify EstimateTransientBytes sums correctly across MRT attachments.
TEST(RenderGraphTransientPool, PhaseD_MRTEstimatedBytesAreCorrect)
{
    RGResourceDesc oitDesc;
    oitDesc.Kind = ResourceHandle::Kind::Framebuffer;
    oitDesc.Width = 1280;
    oitDesc.Height = 720;
    oitDesc.Attachments = { RGResourceFormat::RGBA16Float, RGResourceFormat::RG16Float, RGResourceFormat::Depth24Stencil8 };

    // RGBA16F = 8 bytes/px, RG16F = 4 bytes/px, DEPTH24_STENCIL8 = 4 bytes/px
    // → (8+4+4) * 1280 * 720.
    // Use the public IsTransientDescriptorAllocatable + transient plan approach
    // by registering and building.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto h = graph.DeclareTransientFramebuffer("MRTEstimate", oitDesc);
    AddSetupNode(graph, "Writer", [h](RGBuilder& builder)
                 { builder.Write(h, RGWriteUsage::RenderTarget); });
    AddSetupNode(graph, "Reader", [h](RGBuilder& builder)
                 { [[maybe_unused]] const auto readHandle = builder.Read(h, RGReadUsage::ShaderSample); });
    graph.AddExecutionDependency("Writer", "Reader");
    graph.SetFinalPass("Reader");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& e)
                                         { return e.Resource == "MRTEstimate"; });
    ASSERT_NE(it, plan.end());
    EXPECT_EQ(it->EstimatedBytes, (8ull + 4ull + 4ull) * 1280ull * 720ull)
        << "RGBA16F+RG16F+Depth MRT estimated bytes must sum to 16 bytes/px";
}

// =============================================================================
// Phase D Slice 8 — Post-process chain outputs as transient FBs
//
// Each post-process pass output (BloomColor, DOFColor, ToneMapColor, etc.) is
// now declared as a transient framebuffer rather than imported, so the graph's
// transient pool can track lifetime and format for future aliasing.

TEST(RenderGraphTransientPool, PhaseD_VelocityDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc velocityDesc;
    velocityDesc.Kind = ResourceHandle::Kind::Texture2D;
    velocityDesc.Format = RGResourceFormat::RG16Float;
    velocityDesc.Width = 1280u;
    velocityDesc.Height = 720u;
    const auto velocityHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::Velocity), velocityDesc);

    AddSetupNode(
        graph,
        "ScenePass",
        [velocityHandle](RGBuilder& builder)
        {
            builder.Write(velocityHandle, RGWriteUsage::TransferDest);
        });

    AddSetupNode(
        graph,
        "TAAPass",
        [velocityHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto velocityRead = builder.Read(velocityHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("ScenePass", "TAAPass");
    graph.SetFinalPass("TAAPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::Velocity; });

    ASSERT_NE(it, plan.end()) << "Velocity not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "Velocity must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "Velocity must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "Velocity unexpected skip reason: " << it->SkipReason;
    EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 4ull)
        << "Velocity (RG16F) should be 4 bytes per texel";

    const auto handle = graph.GetTextureHandle(std::string(ResourceNames::Velocity));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for Velocity must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_SceneDepthDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc depthDesc;
    depthDesc.Kind = ResourceHandle::Kind::Texture2D;
    depthDesc.Format = RGResourceFormat::Depth24Stencil8;
    depthDesc.Width = 1280u;
    depthDesc.Height = 720u;
    const auto depthHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::SceneDepth), depthDesc);

    AddSetupNode(
        graph,
        "ScenePass",
        [depthHandle](RGBuilder& builder)
        {
            builder.Write(depthHandle, RGWriteUsage::TransferDest);
        });

    AddSetupNode(
        graph,
        "DOFPass",
        [depthHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto depthRead = builder.Read(depthHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("ScenePass", "DOFPass");
    graph.SetFinalPass("DOFPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::SceneDepth; });

    ASSERT_NE(it, plan.end()) << "SceneDepth not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "SceneDepth must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "SceneDepth must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "SceneDepth unexpected skip reason: " << it->SkipReason;
    EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 4ull)
        << "SceneDepth (Depth24Stencil8) should be 4 bytes per texel";

    const auto handle = graph.GetTextureHandle(std::string(ResourceNames::SceneDepth));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for SceneDepth must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_SceneNormalsDeclaredAsTransientTexture)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc normalsDesc;
    normalsDesc.Kind = ResourceHandle::Kind::Texture2D;
    normalsDesc.Format = RGResourceFormat::RGBA16Float;
    normalsDesc.Width = 1280u;
    normalsDesc.Height = 720u;
    const auto normalsHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::SceneNormals), normalsDesc);

    AddSetupNode(
        graph,
        "DeferredOpaqueDecalPass",
        [normalsHandle](RGBuilder& builder)
        {
            builder.Write(normalsHandle, RGWriteUsage::TransferDest);
        });

    AddSetupNode(
        graph,
        "SSAOPass",
        [normalsHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto normalsRead = builder.Read(normalsHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("DeferredOpaqueDecalPass", "SSAOPass");
    graph.SetFinalPass("SSAOPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::SceneNormals; });

    ASSERT_NE(it, plan.end()) << "SceneNormals not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "SceneNormals must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "SceneNormals must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "SceneNormals unexpected skip reason: " << it->SkipReason;
    EXPECT_EQ(it->EstimatedBytes, 1280ull * 720ull * 8ull)
        << "SceneNormals (RGBA16F deferred path) should be 8 bytes per texel";

    const auto handle = graph.GetTextureHandle(std::string(ResourceNames::SceneNormals));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for SceneNormals must be valid after BuildFrameGraph";
}

TEST(RenderGraphTransientPool, PhaseD_DeferredGBufferRootsDeclaredAsTransientTextures)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc albedoDesc;
    albedoDesc.Kind = ResourceHandle::Kind::Texture2D;
    albedoDesc.Format = RGResourceFormat::RGBA8UNorm;
    albedoDesc.Width = 1280u;
    albedoDesc.Height = 720u;
    const auto albedoHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::GBufferAlbedo), albedoDesc);

    RGResourceDesc normalDesc;
    normalDesc.Kind = ResourceHandle::Kind::Texture2D;
    normalDesc.Format = RGResourceFormat::RGBA16Float;
    normalDesc.Width = 1280u;
    normalDesc.Height = 720u;
    const auto normalHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::GBufferNormal), normalDesc);

    RGResourceDesc emissiveDesc = normalDesc;
    const auto emissiveHandle = graph.AllocateTransientTextureHandle(std::string(ResourceNames::GBufferEmissive), emissiveDesc);

    AddSetupNode(
        graph,
        "DeferredOpaqueDecalPass",
        [albedoHandle, normalHandle, emissiveHandle](RGBuilder& builder)
        {
            builder.Write(albedoHandle, RGWriteUsage::TransferDest);
            builder.Write(normalHandle, RGWriteUsage::TransferDest);
            builder.Write(emissiveHandle, RGWriteUsage::TransferDest);
        });

    AddSetupNode(
        graph,
        "DeferredLightingPass",
        [albedoHandle, normalHandle, emissiveHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto albedoRead = builder.Read(albedoHandle, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalRead = builder.Read(normalHandle, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto emissiveRead = builder.Read(emissiveHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
    graph.SetFinalPass("DeferredLightingPass");
    graph.BuildFrameGraph();

    struct ExpectedResource
    {
        std::string_view Name;
        u64 EstimatedBytes;
    };

    const std::array<ExpectedResource, 3> expected = { {
        { ResourceNames::GBufferAlbedo, 1280ull * 720ull * 4ull },
        { ResourceNames::GBufferNormal, 1280ull * 720ull * 8ull },
        { ResourceNames::GBufferEmissive, 1280ull * 720ull * 8ull },
    } };

    const auto& plan = graph.GetTransientPlan();
    for (const auto& resource : expected)
    {
        const auto it = std::ranges::find_if(plan,
                                             [&resource](const RenderGraph::TransientPlanEntry& entry)
                                             { return entry.Resource == resource.Name; });

        ASSERT_NE(it, plan.end()) << resource.Name << " not found in transient plan";
        EXPECT_TRUE(it->Reachable) << resource.Name << " must be reachable";
        EXPECT_TRUE(it->WillAllocate) << resource.Name << " must be planned for allocation";
        EXPECT_EQ(it->SkipReason, "") << resource.Name << " unexpected skip reason: " << it->SkipReason;
        EXPECT_EQ(it->EstimatedBytes, resource.EstimatedBytes)
            << resource.Name << " estimated bytes mismatch";

        const auto handle = graph.GetTextureHandle(std::string(resource.Name));
        EXPECT_TRUE(handle.IsValid()) << "stable handle for " << resource.Name << " must be valid after BuildFrameGraph";
    }
}

TEST(RenderGraphTransientPool, PhaseD_DeferredMSCompanionsDeclaredAsTransientTextures)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto declareMS = [&graph](std::string_view name, const RGResourceFormat format) -> RGTextureHandle
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Texture2D;
        desc.Format = format;
        desc.Width = 1280u;
        desc.Height = 720u;
        desc.Samples = 4u;
        return graph.AllocateTransientTextureHandle(std::string(name), desc);
    };

    const auto albedoMSHandle = declareMS(ResourceNames::GBufferAlbedoMS, RGResourceFormat::RGBA8UNorm);
    const auto normalMSHandle = declareMS(ResourceNames::GBufferNormalMS, RGResourceFormat::RGBA16Float);
    const auto emissiveMSHandle = declareMS(ResourceNames::GBufferEmissiveMS, RGResourceFormat::RGBA16Float);
    const auto velocityMSHandle = declareMS(ResourceNames::VelocityMS, RGResourceFormat::RG16Float);
    const auto depthMSHandle = declareMS(ResourceNames::SceneDepthMS, RGResourceFormat::Depth24Stencil8);

    AddSetupNode(
        graph,
        "DeferredOpaqueDecalPass",
        [albedoMSHandle, normalMSHandle, emissiveMSHandle, velocityMSHandle, depthMSHandle](RGBuilder& builder)
        {
            builder.Write(albedoMSHandle, RGWriteUsage::TransferDest);
            builder.Write(normalMSHandle, RGWriteUsage::TransferDest);
            builder.Write(emissiveMSHandle, RGWriteUsage::TransferDest);
            builder.Write(velocityMSHandle, RGWriteUsage::TransferDest);
            builder.Write(depthMSHandle, RGWriteUsage::TransferDest);
        });

    AddSetupNode(
        graph,
        "DeferredLightingPass",
        [albedoMSHandle, normalMSHandle, emissiveMSHandle, velocityMSHandle, depthMSHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto albedoRead = builder.Read(albedoMSHandle, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalRead = builder.Read(normalMSHandle, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto emissiveRead = builder.Read(emissiveMSHandle, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto velocityRead = builder.Read(velocityMSHandle, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto depthRead = builder.Read(depthMSHandle, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
    graph.SetFinalPass("DeferredLightingPass");
    graph.BuildFrameGraph();

    struct ExpectedResource
    {
        std::string_view Name;
        u64 EstimatedBytes;
    };

    const std::array<ExpectedResource, 5> expected = { {
        { ResourceNames::GBufferAlbedoMS, 1280ull * 720ull * 4ull * 4ull },
        { ResourceNames::GBufferNormalMS, 1280ull * 720ull * 8ull * 4ull },
        { ResourceNames::GBufferEmissiveMS, 1280ull * 720ull * 8ull * 4ull },
        { ResourceNames::VelocityMS, 1280ull * 720ull * 4ull * 4ull },
        { ResourceNames::SceneDepthMS, 1280ull * 720ull * 4ull * 4ull },
    } };

    const auto& plan = graph.GetTransientPlan();
    for (const auto& resource : expected)
    {
        const auto it = std::ranges::find_if(plan,
                                             [&resource](const RenderGraph::TransientPlanEntry& entry)
                                             { return entry.Resource == resource.Name; });

        ASSERT_NE(it, plan.end()) << resource.Name << " not found in transient plan";
        EXPECT_TRUE(it->Reachable) << resource.Name << " must be reachable";
        EXPECT_TRUE(it->WillAllocate) << resource.Name << " must be planned for allocation";
        EXPECT_EQ(it->SkipReason, "") << resource.Name << " unexpected skip reason: " << it->SkipReason;
        EXPECT_EQ(it->EstimatedBytes, resource.EstimatedBytes)
            << resource.Name << " estimated bytes mismatch";

        const auto handle = graph.GetTextureHandle(std::string(resource.Name));
        EXPECT_TRUE(handle.IsValid()) << "stable handle for " << resource.Name << " must be valid after BuildFrameGraph";
    }
}

TEST(RenderGraphTransientPool, PhaseD_SceneColorDeclaredAsTransientMRTFramebuffer)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc sceneDesc;
    sceneDesc.Kind = ResourceHandle::Kind::Framebuffer;
    sceneDesc.Width = 1280u;
    sceneDesc.Height = 720u;
    sceneDesc.Attachments = {
        RGResourceFormat::RGBA16Float,
        RGResourceFormat::R32Int,
        RGResourceFormat::RG16Float,
        RGResourceFormat::RG16Float,
        RGResourceFormat::Depth24Stencil8,
    };
    const auto sceneHandle = graph.DeclareTransientFramebuffer(std::string(ResourceNames::SceneColor), sceneDesc);

    AddSetupNode(
        graph,
        "ScenePass",
        [sceneHandle](RGBuilder& builder)
        {
            builder.Write(sceneHandle, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "WaterPass",
        [sceneHandle](RGBuilder& builder)
        {
            [[maybe_unused]] const auto sceneRead = builder.Read(sceneHandle, RGReadUsage::RenderTargetRead);
            builder.Write(sceneHandle, RGWriteUsage::RenderTarget);
        });

    graph.AddExecutionDependency("ScenePass", "WaterPass");
    graph.SetFinalPass("WaterPass");
    graph.BuildFrameGraph();

    const auto& plan = graph.GetTransientPlan();
    const auto it = std::ranges::find_if(plan,
                                         [](const RenderGraph::TransientPlanEntry& entry)
                                         { return entry.Resource == ResourceNames::SceneColor; });

    ASSERT_NE(it, plan.end()) << "SceneColor not found in transient plan";
    EXPECT_TRUE(it->Reachable) << "SceneColor must be reachable";
    EXPECT_TRUE(it->WillAllocate) << "SceneColor must be planned for allocation";
    EXPECT_EQ(it->SkipReason, "") << "SceneColor unexpected skip reason: " << it->SkipReason;
    EXPECT_EQ(it->EstimatedBytes, (8ull + 4ull + 4ull + 4ull + 4ull) * 1280ull * 720ull)
        << "SceneColor MRT estimated bytes should sum all attachments";

    const auto handle = graph.GetFramebufferHandle(std::string(ResourceNames::SceneColor));
    EXPECT_TRUE(handle.IsValid()) << "stable handle for SceneColor must be valid after BuildFrameGraph";
}
// =============================================================================

TEST(RenderGraphTransientPool, PhaseD_PostProcessChainRGBA16FOutputsDeclaredAsTransient)
{
    // Verify that each RGBA16F post-process output declared via
    // DeclareTransientFramebuffer gets WillAllocate = true when it has
    // a producer pass in the graph.  Each output gets its own write pass
    // to mirror the production pattern where every pp pass writes its target.
    constexpr u32 vw = 1280;
    constexpr u32 vh = 720;

    const std::vector<std::string> hdOutputs = {
        "SSSColor", "AOApplyColor", "BloomColor", "DOFColor",
        "MotionBlurColor", "TAAColor", "PrecipitationColor", "FogColor",
        "ChromAbColor", "ColorGradingColor", "ToneMapColor"
    };

    for (const auto& name : hdOutputs)
    {
        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(false);

        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = vw;
        desc.Height = vh;
        desc.Format = RGResourceFormat::RGBA16Float;
        desc.DebugName = name;
        const auto h = graph.DeclareTransientFramebuffer(name, desc);
        ASSERT_TRUE(h.IsValid()) << name << ": handle must be valid after declaration";

        AddSetupNode(graph, "Writer", [h](RGBuilder& builder)
                     { builder.Write(h, RGWriteUsage::RenderTarget); });
        AddSetupNode(graph, "Reader", [h](RGBuilder& builder)
                     { [[maybe_unused]] const auto readHandle = builder.Read(h, RGReadUsage::ShaderSample); });
        graph.AddExecutionDependency("Writer", "Reader");
        graph.SetFinalPass("Reader");
        graph.BuildFrameGraph();

        const auto& plan = graph.GetTransientPlan();
        const auto it = std::ranges::find_if(plan,
                                             [&](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == name; });
        ASSERT_NE(it, plan.end()) << name << " must appear in the transient plan";
        EXPECT_TRUE(it->WillAllocate) << name << " must have WillAllocate = true";
        // RGBA16F: 8 bytes/texel
        EXPECT_EQ(it->EstimatedBytes, static_cast<u64>(vw) * vh * 8u) << name;
    }
}

TEST(RenderGraphTransientPool, PhaseD_PostProcessChainRGBA8OutputsDeclaredAsTransient)
{
    // Verify that each RGBA8 post-process output declared via
    // DeclareTransientFramebuffer gets WillAllocate = true when it has
    // a producer pass in the graph.
    constexpr u32 vw = 1920;
    constexpr u32 vh = 1080;

    const std::vector<std::string> ldOutputs = {
        "VignetteColor", "FXAAColor", "SelectionOutlineColor", "UIComposite"
    };

    for (const auto& name : ldOutputs)
    {
        RenderGraph graph;
        graph.SetRuntimeBarrierExecutionEnabled(false);

        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = vw;
        desc.Height = vh;
        desc.Format = RGResourceFormat::RGBA8UNorm;
        desc.DebugName = name;
        const auto h = graph.DeclareTransientFramebuffer(name, desc);
        ASSERT_TRUE(h.IsValid()) << name << ": handle must be valid after declaration";

        AddSetupNode(graph, "Writer", [h](RGBuilder& builder)
                     { builder.Write(h, RGWriteUsage::RenderTarget); });
        AddSetupNode(graph, "Reader", [h](RGBuilder& builder)
                     { [[maybe_unused]] const auto readHandle = builder.Read(h, RGReadUsage::ShaderSample); });
        graph.AddExecutionDependency("Writer", "Reader");
        graph.SetFinalPass("Reader");
        graph.BuildFrameGraph();

        const auto& plan = graph.GetTransientPlan();
        const auto it = std::ranges::find_if(plan,
                                             [&](const RenderGraph::TransientPlanEntry& e)
                                             { return e.Resource == name; });
        ASSERT_NE(it, plan.end()) << name << " must appear in the transient plan";
        EXPECT_TRUE(it->WillAllocate) << name << " must have WillAllocate = true";
        // RGBA8UNorm: 4 bytes/texel
        EXPECT_EQ(it->EstimatedBytes, static_cast<u64>(vw) * vh * 4u) << name;
    }
}

TEST(RenderGraphTransientPool, PhaseD_PostProcessTransientNotInImportedResources)
{
    // Post-process outputs declared via DeclareTransientFramebuffer must NOT
    // be flagged as IsImported in resource lifetime records — they are
    // graph-owned transients, not externally-owned imports.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    constexpr u32 vw = 1280;
    constexpr u32 vh = 720;

    struct PassSpec
    {
        std::string Name;
        RGResourceFormat Format;
    };
    const std::vector<PassSpec> specs = {
        { "SSSColor", RGResourceFormat::RGBA16Float },
        { "AOApplyColor", RGResourceFormat::RGBA16Float },
        { "BloomColor", RGResourceFormat::RGBA16Float },
        { "DOFColor", RGResourceFormat::RGBA16Float },
        { "MotionBlurColor", RGResourceFormat::RGBA16Float },
        { "TAAColor", RGResourceFormat::RGBA16Float },
        { "PrecipitationColor", RGResourceFormat::RGBA16Float },
        { "FogColor", RGResourceFormat::RGBA16Float },
        { "ToneMapColor", RGResourceFormat::RGBA16Float },
        { "FXAAColor", RGResourceFormat::RGBA8UNorm },
        { "UIComposite", RGResourceFormat::RGBA8UNorm },
    };

    // Declare all resources and wire a single write pass per resource so
    // each one is reachable in the graph and appears in lifetime records.
    std::vector<RGFramebufferHandle> handles;
    for (const auto& s : specs)
    {
        RGResourceDesc desc;
        desc.Kind = ResourceHandle::Kind::Framebuffer;
        desc.Width = vw;
        desc.Height = vh;
        desc.Format = s.Format;
        desc.DebugName = s.Name;
        handles.push_back(graph.DeclareTransientFramebuffer(s.Name, desc));
        EXPECT_TRUE(handles.back().IsValid()) << s.Name << " handle must be valid after declaration";
    }

    // Register a separate writer pass for each resource, and chain them so
    // the graph has a single final pass.
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto h = handles[i];
        const std::string passName = "Writer" + std::to_string(i);
        AddSetupNode(graph, passName, [h](RGBuilder& builder)
                     { builder.Write(h, RGWriteUsage::RenderTarget); });
    }
    for (std::size_t i = 1; i < specs.size(); ++i)
    {
        graph.AddExecutionDependency("Writer" + std::to_string(i - 1),
                                     "Writer" + std::to_string(i));
    }
    graph.SetFinalPass("Writer" + std::to_string(specs.size() - 1));
    graph.BuildFrameGraph();

    // Use resource lifetime records (Phase G Slice 11 API) to confirm IsImported == false.
    const auto lifetimes = graph.GetResourceLifetimes();
    for (const auto& s : specs)
    {
        const auto it = std::ranges::find_if(lifetimes,
                                             [&](const RenderGraph::ResourceLifetime& l)
                                             { return l.ResourceName == s.Name; });
        if (it != lifetimes.end())
        {
            EXPECT_FALSE(it->IsImported)
                << s.Name << " must NOT be flagged IsImported when declared as transient";
            EXPECT_TRUE(it->IsTransient)
                << s.Name << " must be flagged IsTransient";
        }
    }
}

TEST(RenderGraphTransientPool, PhaseH_ScratchTransientsRemainGraphOwned)
{
    // Scratch resources that support optional fullscreen stages must stay
    // graph-owned transients so passes cannot silently drift back to hidden
    // owner-backed side paths.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    constexpr u32 vw = 1280;
    constexpr u32 vh = 720;

    struct ScratchSpec
    {
        std::string Name;
        ResourceHandle::Kind Kind;
        RGResourceFormat Format;
        u32 Width;
        u32 Height;
    };

    const std::vector<ScratchSpec> specs = {
        { "SSAORaw", ResourceHandle::Kind::Framebuffer, RGResourceFormat::RG16Float, vw / 2, vh / 2 },
        { std::string(ResourceNames::SSAOBlur), ResourceHandle::Kind::Framebuffer, RGResourceFormat::RG16Float, vw / 2, vh / 2 },
        { "JFAPing", ResourceHandle::Kind::Framebuffer, RGResourceFormat::RGBA32Float, vw, vh },
        { "JFAPong", ResourceHandle::Kind::Framebuffer, RGResourceFormat::RGBA32Float, vw, vh },
        { "BloomMip0", ResourceHandle::Kind::Framebuffer, RGResourceFormat::RGBA16Float, vw / 2, vh / 2 },
        { "FogHalfRes", ResourceHandle::Kind::Framebuffer, RGResourceFormat::RGBA16Float, vw / 2, vh / 2 },
        { "GTAOEdge", ResourceHandle::Kind::Texture2D, RGResourceFormat::R8UNorm, vw, vh },
        { std::string(ResourceNames::GTAODenoisePing), ResourceHandle::Kind::Texture2D, RGResourceFormat::R8UNorm, vw, vh },
        { std::string(ResourceNames::GTAODenoisePong), ResourceHandle::Kind::Texture2D, RGResourceFormat::R8UNorm, vw, vh },
        { "HZBDepth", ResourceHandle::Kind::Texture2D, RGResourceFormat::R32Float, 2048, 1024 },
        { "WaterRefraction", ResourceHandle::Kind::Texture2D, RGResourceFormat::RGBA16Float, vw, vh },
    };

    for (const auto& spec : specs)
    {
        RGResourceDesc desc;
        desc.Kind = spec.Kind;
        desc.Width = spec.Width;
        desc.Height = spec.Height;
        desc.Format = spec.Format;
        desc.DebugName = spec.Name;

        if (spec.Kind == ResourceHandle::Kind::Framebuffer)
        {
            const auto handle = graph.DeclareTransientFramebuffer(spec.Name, desc);
            ASSERT_TRUE(handle.IsValid()) << spec.Name << " handle must be valid after declaration";
        }
        else
        {
            AddSetupNode(graph, spec.Name + "Writer", [name = spec.Name, desc](RGBuilder& builder)
                         {
                             const auto handle = builder.CreateTexture(name, desc);
                             builder.Write(handle, RGWriteUsage::ShaderImage); });
            AddSetupNode(graph, spec.Name + "Reader", [name = spec.Name, desc](RGBuilder& builder)
                         {
                             const auto handle = builder.CreateTexture(name, desc);
                             [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderSample); });
        }
    }

    AddSetupNode(graph, "FBWriter0", [](RGBuilder& builder)
                 {
                     RGResourceDesc desc;
                     desc.Kind = ResourceHandle::Kind::Framebuffer;
                     desc.Width = vw / 2;
                     desc.Height = vh / 2;
                     desc.Format = RGResourceFormat::RG16Float;
                     const auto raw = builder.CreateFramebuffer("SSAORaw", desc);
                     builder.Write(raw, RGWriteUsage::RenderTarget);

                     const auto blur = builder.CreateFramebuffer(std::string(ResourceNames::SSAOBlur), desc);
                     builder.Write(blur, RGWriteUsage::RenderTarget); });
    AddSetupNode(graph, "FBWriter1", [](RGBuilder& builder)
                 {
                     RGResourceDesc desc;
                     desc.Kind = ResourceHandle::Kind::Framebuffer;
                     desc.Width = vw;
                     desc.Height = vh;
                     desc.Format = RGResourceFormat::RGBA32Float;
                     const auto handle = builder.CreateFramebuffer("JFAPing", desc);
                     builder.Write(handle, RGWriteUsage::RenderTarget); });
    AddSetupNode(graph, "FBWriter2", [](RGBuilder& builder)
                 {
                     RGResourceDesc desc;
                     desc.Kind = ResourceHandle::Kind::Framebuffer;
                     desc.Width = vw;
                     desc.Height = vh;
                     desc.Format = RGResourceFormat::RGBA32Float;
                     const auto ping = builder.CreateFramebuffer("JFAPong", desc);
                     builder.Write(ping, RGWriteUsage::RenderTarget);

                     desc.Width = vw / 2;
                     desc.Height = vh / 2;
                     desc.Format = RGResourceFormat::RGBA16Float;
                     const auto bloom = builder.CreateFramebuffer("BloomMip0", desc);
                     builder.Write(bloom, RGWriteUsage::RenderTarget); });
    AddSetupNode(graph, "FBWriterFogHalfRes", [](RGBuilder& builder)
                 {
                     RGResourceDesc desc;
                     desc.Kind = ResourceHandle::Kind::Framebuffer;
                     desc.Width = vw / 2;
                     desc.Height = vh / 2;
                     desc.Format = RGResourceFormat::RGBA16Float;
                     const auto fogHalf = builder.CreateFramebuffer("FogHalfRes", desc);
                     builder.Write(fogHalf, RGWriteUsage::RenderTarget); });

    graph.AddExecutionDependency("FBWriter0", "FBWriter1");
    graph.AddExecutionDependency("FBWriter1", "FBWriter2");
    graph.AddExecutionDependency("FBWriter2", "FBWriterFogHalfRes");
    graph.AddExecutionDependency("GTAOEdgeWriter", "GTAOEdgeReader");
    graph.AddExecutionDependency("HZBDepthWriter", "HZBDepthReader");
    graph.AddExecutionDependency("WaterRefractionWriter", "WaterRefractionReader");
    graph.AddExecutionDependency("FBWriterFogHalfRes", "GTAOEdgeWriter");
    graph.AddExecutionDependency("GTAOEdgeReader", "GTAODenoisePingWriter");
    graph.AddExecutionDependency("GTAODenoisePingWriter", "GTAODenoisePingReader");
    graph.AddExecutionDependency("GTAODenoisePingReader", "GTAODenoisePongWriter");
    graph.AddExecutionDependency("GTAODenoisePongWriter", "GTAODenoisePongReader");
    graph.AddExecutionDependency("GTAODenoisePongReader", "HZBDepthWriter");
    graph.AddExecutionDependency("HZBDepthReader", "WaterRefractionWriter");
    graph.SetFinalPass("WaterRefractionReader");
    graph.BuildFrameGraph();

    const auto lifetimes = graph.GetResourceLifetimes();
    for (const auto& spec : specs)
    {
        const auto it = std::ranges::find_if(lifetimes,
                                             [&](const RenderGraph::ResourceLifetime& lifetime)
                                             { return lifetime.ResourceName == spec.Name; });
        ASSERT_NE(it, lifetimes.end()) << spec.Name << " must appear in lifetime records";
        EXPECT_FALSE(it->IsImported) << spec.Name << " must remain graph-owned, not imported";
        EXPECT_TRUE(it->IsTransient) << spec.Name << " must remain transient";
    }
}

TEST(RenderGraphTransientPool, TrimMaxBucketSizeDefaultIsTwo)
{
    // Default max-bucket-size is 2: tolerates one spare for same-descriptor
    // overlapping transients without unbounded pool growth.
    RenderGraph graph;
    EXPECT_EQ(graph.GetTransientPoolMaxBucketSize(), 2u);
}

TEST(RenderGraphTransientPool, SetTransientPoolMaxBucketSizeRoundTrips)
{
    RenderGraph graph;
    graph.SetTransientPoolMaxBucketSize(1u);
    EXPECT_EQ(graph.GetTransientPoolMaxBucketSize(), 1u);
    graph.SetTransientPoolMaxBucketSize(4u);
    EXPECT_EQ(graph.GetTransientPoolMaxBucketSize(), 4u);
}

TEST(RenderGraphTransientPool, TrimOnEmptyPoolIsNoop)
{
    // Calling Trim on a pool that has no objects must not crash or corrupt state.
    TransientPool pool;
    pool.Trim(1u);
    const auto stats = pool.GetStats();
    EXPECT_EQ(stats.TexturePoolSize, 0u);
    EXPECT_EQ(stats.FramebufferPoolSize, 0u);
    EXPECT_EQ(stats.BufferPoolSize, 0u);
    EXPECT_EQ(pool.EstimateMemoryUsage(), 0u);
}

// =============================================================================
// Phase G — Slice 1: pass work-type classification
//
// These tests document the behavioural contracts for PassWorkType,
// AsyncComputeCandidate, and NeverCull introduced in Phase G.
// =============================================================================

TEST(RenderGraphPassFlags, PassWorkTypeDefaultsToGraphics)
{
    // Passes that do not call SetPassWorkType should default to Graphics.
    RenderGraph graph;
    auto pass = AddStub(graph, "SomePass");
    EXPECT_EQ(pass->GetPassWorkType(), RenderGraphNode::PassWorkType::Graphics);
    EXPECT_FALSE(pass->IsComputeOnly());
}

TEST(RenderGraphPassFlags, ComputePassTypeRoundTrips)
{
    // A pass explicitly flagged as Compute must report IsComputeOnly().
    RenderGraph graph;
    auto pass = AddStub(graph, "ComputePass");
    pass->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    EXPECT_EQ(pass->GetPassWorkType(), RenderGraphNode::PassWorkType::Compute);
    EXPECT_TRUE(pass->IsComputeOnly());
}

TEST(RenderGraphPassFlags, CopyPassTypeRoundTrips)
{
    // A pass flagged as Copy must report Copy and must not be compute-only.
    RenderGraph graph;
    auto pass = AddStub(graph, "CopyPass");
    pass->SetPassWorkType(RenderGraphNode::PassWorkType::Copy);
    EXPECT_EQ(pass->GetPassWorkType(), RenderGraphNode::PassWorkType::Copy);
    EXPECT_FALSE(pass->IsComputeOnly());
}

TEST(RenderGraphPassFlags, AsyncComputeCandidateFlagRoundTrips)
{
    // SetAsyncComputeCandidate must be reflected in IsAsyncComputeCandidate.
    RenderGraph graph;
    auto pass = AddStub(graph, "AsyncPass");
    EXPECT_FALSE(pass->IsAsyncComputeCandidate());
    pass->SetAsyncComputeCandidate(true);
    EXPECT_TRUE(pass->IsAsyncComputeCandidate());
    pass->SetAsyncComputeCandidate(false);
    EXPECT_FALSE(pass->IsAsyncComputeCandidate());
}

TEST(RenderGraphPassFlags, NeverCullPreventsCulling)
{
    // An unreachable pass marked NeverCull must still execute.
    RenderGraph graph;
    auto a = AddStub(graph, "A");
    auto b = AddStub(graph, "B");
    auto isolated = AddStub(graph, "NeverCullPass");

    graph.ConnectPass("A", "B");
    graph.SetFinalPass("B");

    // NeverCull is a SideEffect bit — IsSideEffecting() returns true,
    // which prevents ComputeReachability() from culling the pass.
    isolated->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(isolated->GetExecuteCount(), 1u) << "NeverCull pass must not be culled";
}

TEST(RenderGraphPassFlags, NodeSubmissionInfoReportsWorkTypeAndAsyncFlag)
{
    // GetNodeSubmissionInfo() must surface PassWorkType and AsyncComputeCandidate.
    RenderGraph graph;
    auto graphics = AddStub(graph, "GraphicsPass");
    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);

    graph.SetFinalPass("GraphicsPass");

    const auto infos = graph.GetNodeSubmissionInfo();
    ASSERT_FALSE(infos.empty());

    for (const auto& info : infos)
    {
        if (info.NodeName == "GraphicsPass")
        {
            EXPECT_EQ(info.WorkType, RenderGraphPassWorkType::Graphics);
            EXPECT_FALSE(info.AsyncComputeCandidate);
        }
        else if (info.NodeName == "ComputePass")
        {
            EXPECT_EQ(info.WorkType, RenderGraphPassWorkType::Compute);
            EXPECT_TRUE(info.AsyncComputeCandidate);
        }
        else
        {
            // No additional handling required.
        }
    }
}

// =============================================================================
// RenderGraphComputeHoist — Phase G Slice 2: compute-pass scheduling hoist
// =============================================================================

TEST(RenderGraphComputeHoist, NoCandidatesLeavesOrderUnchanged)
{
    // When no pass is marked AsyncComputeCandidate the hoist is a no-op and
    // the insertion-order-stable topological sort is preserved.
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");
    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");
    graph.SetFinalPass("C");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "B");
    EXPECT_EQ(order[2], "C");
}

TEST(RenderGraphComputeHoist, IndependentComputePassIsHoistedToFront)
{
    // G1 -> G2 (graphics chain). C is an AsyncComputeCandidate with no
    // dependency on G1 or G2. After the hoist C must appear before G1 and G2
    // because all three have their constraints satisfied from the start.
    RenderGraph graph;
    AddStub(graph, "G1");
    AddStub(graph, "G2");
    auto c = AddStub(graph, "C");
    c->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c->SetAsyncComputeCandidate(true);

    graph.ConnectPass("G1", "G2");
    graph.SetFinalPass("G2");
    c->SetSideEffects(RenderGraphNode::SideEffect::NeverCull); // keep C alive

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 3u);
    // C must be scheduled before both graphics passes.
    const auto posC = std::ranges::find(order, "C") - order.begin();
    const auto posG1 = std::ranges::find(order, "G1") - order.begin();
    const auto posG2 = std::ranges::find(order, "G2") - order.begin();
    EXPECT_LT(posC, posG1) << "Compute pass should be hoisted before G1";
    EXPECT_LT(posC, posG2) << "Compute pass should be hoisted before G2";
}

TEST(RenderGraphComputeHoist, DependentComputePassRemainsAfterDependency)
{
    // G1 -> C (compute depends on G1). The hoist must respect the dependency:
    // C cannot be scheduled before G1 even though it is an AsyncComputeCandidate.
    RenderGraph graph;
    AddStub(graph, "G1");
    auto c = AddStub(graph, "C");
    c->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c->SetAsyncComputeCandidate(true);

    graph.ConnectPass("G1", "C");
    graph.SetFinalPass("C");

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 2u);
    const auto posG1 = std::ranges::find(order, "G1") - order.begin();
    const auto posC = std::ranges::find(order, "C") - order.begin();
    EXPECT_LT(posG1, posC) << "G1 must still precede its dependent compute pass";
}

TEST(RenderGraphComputeHoist, MultipleComputePassesAllHoisted)
{
    // C1, C2 (both AsyncComputeCandidate, independent), then G1 (graphics).
    // After hoist both compute passes appear before G1.
    RenderGraph graph;
    AddStub(graph, "G1");
    auto c1 = AddStub(graph, "C1");
    auto c2 = AddStub(graph, "C2");
    c1->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    c2->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);

    graph.SetFinalPass("G1");
    c1->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    c2->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 3u);
    const auto posG1 = std::ranges::find(order, "G1") - order.begin();
    const auto posC1 = std::ranges::find(order, "C1") - order.begin();
    const auto posC2 = std::ranges::find(order, "C2") - order.begin();
    EXPECT_LT(posC1, posG1) << "C1 must be hoisted before G1";
    EXPECT_LT(posC2, posG1) << "C2 must be hoisted before G1";
}

// =============================================================================
// RenderGraphDumpJson — Phase G Slice 3: PassWorkType + AsyncComputeCandidate in JSON dump
// =============================================================================

TEST(RenderGraphDumpJson, PassFlagsAreSurfacedInDump)
{
    // A graph with one graphics pass and one async-compute pass must produce
    // schemaVersion 13 JSON containing a passFlags array with correct entries,
    // frameSummary counters, and per-entry workType/asyncComputeCandidate in
    // executionTimeline and timingStatsByPass.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "GfxPass");
    auto computePass = AddStub(graph, "ComputePass");
    computePass->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    computePass->SetAsyncComputeCandidate(true);
    computePass->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    graph.ConnectPass("ComputePass", "GfxPass");
    graph.SetFinalPass("GfxPass");
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "rg_phase_g3_flags.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Schema version bump
    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);

    // frameSummary compute counts
    EXPECT_NE(json.find("\"computePassCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"asyncComputeCandidateCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"historyResourceCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"temporalHistoryContractCount\": 0"), std::string::npos);

    // passFlags array must be present and contain both passes
    EXPECT_NE(json.find("\"passFlags\""), std::string::npos);
    EXPECT_NE(json.find("\"workType\": \"Compute\""), std::string::npos);
    EXPECT_NE(json.find("\"asyncComputeCandidate\": true"), std::string::npos);
    EXPECT_NE(json.find("\"workType\": \"Graphics\""), std::string::npos);
    EXPECT_NE(json.find("\"asyncComputeCandidate\": false"), std::string::npos);

    // executionTimeline entries carry the new fields
    EXPECT_NE(json.find("\"executionTimeline\""), std::string::npos);

    // timingStatsByPass entries carry the new fields
    EXPECT_NE(json.find("\"timingStatsByPass\""), std::string::npos);

    // graphDigest includes compute/asyncCandidates fields
    EXPECT_NE(json.find("compute=1;asyncCandidates=1"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraphDumpJson, GraphDigestContainsComputeCountsForAllGraphicsPasses)
{
    // When no compute pass is present the graphDigest must show compute=0;asyncCandidates=0.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    AddStub(graph, "A");
    AddStub(graph, "B");
    graph.ConnectPass("A", "B");
    graph.SetFinalPass("B");
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "rg_phase_g3_allgfx.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("compute=0;asyncCandidates=0"), std::string::npos);
    EXPECT_NE(json.find("\"computePassCount\": 0"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// =============================================================================
// RenderGraphDumpDot — Phase G Slice 3: compute-pass coloring in DOT dump
// =============================================================================

TEST(RenderGraphDumpDot, ComputePassColoredDifferentlyToGraphics)
{
    // A compute pass must get amber fill (#fff3cd) instead of the default blue (#e8f0fe).
    // An async-compute candidate must have its label prefixed with "[async] ".
    RenderGraph graph;
    AddStub(graph, "GfxPass");
    auto computePass = AddStub(graph, "ComputePass");
    computePass->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    computePass->SetAsyncComputeCandidate(true);
    computePass->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    graph.ConnectPass("ComputePass", "GfxPass");
    graph.SetFinalPass("GfxPass");
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "rg_phase_g3_dot.dot";
    ASSERT_TRUE(graph.DumpToDot(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string dot((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Compute pass must have amber fill
    EXPECT_NE(dot.find("#fff3cd"), std::string::npos) << "Compute pass should have amber fill";

    // Async-compute candidate label prefix
    EXPECT_NE(dot.find("[async] ComputePass"), std::string::npos) << "Async candidate label must be prefixed";

    // Batch lane annotations (Phase G.13)
    EXPECT_NE(dot.find("batch 0: lane=Compute"), std::string::npos)
        << "DOT dump must surface stable lane assignment per batch";

    // Graphics pass must NOT have the amber fill
    const auto gfxPos = dot.find("\"GfxPass\"");
    ASSERT_NE(gfxPos, std::string::npos);
    // The graphics pass node entry (double-ring final) should not contain #fff3cd
    // — search the attribute block that follows "GfxPass" up to the next semicolon
    if (const auto semiPos = dot.find(';', gfxPos); semiPos != std::string::npos)
    {
        const auto nodeEntry = dot.substr(gfxPos, semiPos - gfxPos);
        EXPECT_EQ(nodeEntry.find("#fff3cd"), std::string::npos)
            << "Graphics final pass should not have amber fill";
    }

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// =============================================================================
// RenderGraphAsyncBatch — Phase G Slice 4: GetAsyncComputeBatches()
// =============================================================================

TEST(RenderGraphAsyncBatch, NoCandidatesReturnsEmptyBatches)
{
    // A pure-graphics graph must return an empty batch list.
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    graph.ConnectPass("A", "B");
    graph.SetFinalPass("B");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    EXPECT_TRUE(batches.empty());
}

TEST(RenderGraphAsyncBatch, SingleComputePassFormsBatchWithCorrectSignalPass)
{
    // Graph: Compute → GfxPass (final).
    // Batch: {ComputePass}, WaitNodes={}, SignalNodes={GfxPass}.
    RenderGraph graph;
    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxPass");

    graph.ConnectPass("ComputePass", "GfxPass");
    graph.SetFinalPass("GfxPass");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_EQ(batch.Lane, RenderGraph::QueueLane::Compute)
        << "Async compute batches must be assigned to compute lane";
    ASSERT_EQ(batch.ComputeNodes.size(), 1u);
    EXPECT_EQ(batch.ComputeNodes[0], "ComputePass");

    // ComputePass has no non-batch predecessors
    EXPECT_TRUE(batch.WaitNodes.empty());

    // GfxPass depends on ComputePass — must appear in SignalNodes
    ASSERT_EQ(batch.SignalNodes.size(), 1u);
    EXPECT_EQ(batch.SignalNodes[0], "GfxPass");
}

TEST(RenderGraphAsyncBatch, IndependentComputePassHasEmptyWaitAndSignalLists)
{
    // ComputePass is NeverCull but has no edges to/from GfxFinal.
    // Both WaitNodes and SignalNodes must be empty.
    RenderGraph graph;
    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_EQ(batch.ComputeNodes.size(), 1u);
    EXPECT_TRUE(batch.WaitNodes.empty());
    EXPECT_TRUE(batch.SignalNodes.empty());
}

TEST(RenderGraphAsyncBatch, ConsecutiveComputePassesGroupedInOneBatch)
{
    // Graph: C1 → C2 (both async compute) → GfxFinal.
    // C1 and C2 are consecutive in the hoisted order — one batch.
    RenderGraph graph;
    auto c1 = AddStub(graph, "C1");
    c1->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    c1->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    auto c2 = AddStub(graph, "C2");
    c2->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);
    c2->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    AddStub(graph, "GfxFinal");

    graph.ConnectPass("C1", "C2");
    graph.ConnectPass("C2", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u) << "C1 and C2 are consecutive — one batch expected";

    const auto& batch = batches[0];
    EXPECT_EQ(batch.ComputeNodes.size(), 2u);

    // GfxFinal depends on C2 — must be in SignalNodes
    ASSERT_EQ(batch.SignalNodes.size(), 1u);
    EXPECT_EQ(batch.SignalNodes[0], "GfxFinal");
}

TEST(RenderGraphAsyncBatch, ComputeBatchWaitsForGraphicsPrerequisite)
{
    // Graph: GfxPre → Compute → GfxPost (final).
    // Batch WaitNodes must contain GfxPre; SignalNodes must contain GfxPost.
    RenderGraph graph;
    AddStub(graph, "GfxPre");

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);

    AddStub(graph, "GfxPost");

    graph.ConnectPass("GfxPre", "ComputePass");
    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    ASSERT_EQ(batch.WaitNodes.size(), 1u);
    EXPECT_EQ(batch.WaitNodes[0], "GfxPre")
        << "ComputePass must list GfxPre as a prerequisite to wait for";

    ASSERT_EQ(batch.SignalNodes.size(), 1u);
    EXPECT_EQ(batch.SignalNodes[0], "GfxPost")
        << "GfxPost must be listed as waiting for the compute batch";
}

// =============================================================================
// RenderGraphSubmissionPlan — Phase G Slice 5: GetSubmissionPlan()
// =============================================================================

namespace
{
    // Helper: count SubmissionCommand entries of a given Kind.
    using SCKind = RenderGraph::SubmissionCommand::Kind;

    u32 CountKind(const std::vector<RenderGraph::SubmissionCommand>& plan, SCKind kind)
    {
        return static_cast<u32>(
            std::ranges::count_if(plan, [kind](const RenderGraph::SubmissionCommand& c)
                                  { return c.CommandKind == kind; }));
    }

    // Helper: collect PassNames for Pass commands in order.
    std::vector<std::string> PassOrder(const std::vector<RenderGraph::SubmissionCommand>& plan)
    {
        std::vector<std::string> names;
        for (const auto& cmd : plan)
        {
            if (cmd.CommandKind == SCKind::Pass)
                names.push_back(cmd.NodeName);
        }
        return names;
    }
} // namespace

TEST(RenderGraphSubmissionPlan, PureGraphicsGraphHasOnlyPassCommands)
{
    // A graph with no compute candidates must yield exactly one Pass command
    // per pass and no BatchBegin/BatchEnd/MemoryBarrier commands.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    AddStub(graph, "A");
    AddStub(graph, "B");
    graph.ConnectPass("A", "B");
    graph.SetFinalPass("B");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();

    EXPECT_EQ(CountKind(plan, SCKind::Pass), 2u);
    EXPECT_EQ(CountKind(plan, SCKind::BatchBegin), 0u);
    EXPECT_EQ(CountKind(plan, SCKind::BatchEnd), 0u);
    EXPECT_EQ(CountKind(plan, SCKind::MemoryBarrier), 0u);

    // Pass ordering must mirror execution order
    const auto passNames = PassOrder(plan);
    ASSERT_EQ(passNames.size(), 2u);
    EXPECT_EQ(passNames[0], "A");
    EXPECT_EQ(passNames[1], "B");
}

TEST(RenderGraphSubmissionPlan, ComputePassWrappedInBatchBeginEnd)
{
    // Graph: Compute → GfxFinal.
    // Plan must contain: BatchBegin, Pass(Compute), BatchEnd, Pass(GfxFinal).
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();

    EXPECT_EQ(CountKind(plan, SCKind::BatchBegin), 1u);
    EXPECT_EQ(CountKind(plan, SCKind::BatchEnd), 1u);
    EXPECT_EQ(CountKind(plan, SCKind::Pass), 2u);

    // BatchBegin must appear before the compute Pass, BatchEnd after it but before GfxFinal
    auto beginIt = std::ranges::find_if(plan,
                                        [](const auto& c)
                                        { return c.CommandKind == SCKind::BatchBegin; });
    auto endIt = std::ranges::find_if(plan,
                                      [](const auto& c)
                                      { return c.CommandKind == SCKind::BatchEnd; });
    auto computeIt = std::ranges::find_if(plan,
                                          [](const auto& c)
                                          {
                                              return c.CommandKind == SCKind::Pass && c.NodeName == "ComputePass";
                                          });
    auto gfxIt = std::ranges::find_if(plan,
                                      [](const auto& c)
                                      {
                                          return c.CommandKind == SCKind::Pass && c.NodeName == "GfxFinal";
                                      });

    ASSERT_NE(beginIt, plan.end());
    ASSERT_NE(endIt, plan.end());
    ASSERT_NE(computeIt, plan.end());
    ASSERT_NE(gfxIt, plan.end());

    EXPECT_EQ(beginIt->Lane, RenderGraph::QueueLane::Compute);
    EXPECT_EQ(endIt->Lane, RenderGraph::QueueLane::Compute);

    EXPECT_LT(beginIt, computeIt) << "BatchBegin must precede ComputePass";
    EXPECT_LT(computeIt, endIt) << "ComputePass must precede BatchEnd";
    EXPECT_LT(endIt, gfxIt) << "BatchEnd must precede GfxFinal";
}

TEST(RenderGraphSubmissionPlan, PassCommandsCarryCorrectWorkType)
{
    // Each Pass command must carry the WorkType of the underlying RenderPass.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();

    for (const auto& cmd : plan)
    {
        if (cmd.CommandKind != SCKind::Pass)
            continue;
        if (cmd.NodeName == "ComputePass")
        {
            EXPECT_EQ(cmd.WorkType, RenderGraphPassWorkType::Compute);
            EXPECT_EQ(cmd.Lane, RenderGraph::QueueLane::Compute)
                << "Compute pass must map to compute lane";
        }
        else if (cmd.NodeName == "GfxFinal")
        {
            EXPECT_EQ(cmd.WorkType, RenderGraphPassWorkType::Graphics);
            EXPECT_EQ(cmd.Lane, RenderGraph::QueueLane::Graphics)
                << "Graphics pass must map to graphics lane";
        }
        else
        {
            // No additional handling required.
        }
    }
}

TEST(RenderGraphSubmissionPlan, BatchBeginCarriesWaitAndInputResources)
{
    // Graph: GfxPre(write SharedTex) -> ComputePass(read SharedTex) -> GfxPost.
    // BatchBegin must carry wait/input metadata for backend queue-wait mapping.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                401,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SharedTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "ComputePass",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                401,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SharedTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    AddStub(graph, "GfxPost");
    graph.ConnectPass("GfxPre", "ComputePass");
    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();
    const auto beginIt = std::ranges::find_if(plan,
                                              [](const auto& c)
                                              { return c.CommandKind == SCKind::BatchBegin; });
    ASSERT_NE(beginIt, plan.end());
    EXPECT_EQ(beginIt->Lane, RenderGraph::QueueLane::Compute)
        << "BatchBegin lane should be compute";

    ASSERT_EQ(beginIt->WaitNodes.size(), 1u);
    EXPECT_EQ(beginIt->WaitNodes[0], "GfxPre");

    ASSERT_EQ(beginIt->InputResources.size(), 1u);
    EXPECT_EQ(beginIt->InputResources[0].ResourceName, "SharedTex");
    EXPECT_EQ(beginIt->InputResources[0].ExternalNode, "GfxPre");
}

TEST(RenderGraphSubmissionPlan, BatchEndCarriesSignalAndOutputResources)
{
    // Graph: ComputePass(write ResultTex) -> GfxPost(read ResultTex).
    // BatchEnd must carry signal/output metadata for backend queue-signal mapping.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ComputePass",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata |
            RenderGraphNodeFlags::NeverCull,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                402,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ResultTex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                402,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ResultTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();
    const auto endIt = std::ranges::find_if(plan,
                                            [](const auto& c)
                                            { return c.CommandKind == SCKind::BatchEnd; });
    ASSERT_NE(endIt, plan.end());
    EXPECT_EQ(endIt->Lane, RenderGraph::QueueLane::Compute)
        << "BatchEnd lane should be compute";

    ASSERT_EQ(endIt->SignalNodes.size(), 1u);
    EXPECT_EQ(endIt->SignalNodes[0], "GfxPost");

    ASSERT_EQ(endIt->OutputResources.size(), 1u);
    EXPECT_EQ(endIt->OutputResources[0].ResourceName, "ResultTex");
    EXPECT_EQ(endIt->OutputResources[0].ExternalNode, "GfxPost");
}

TEST(RenderGraphSubmissionPlan, DumpToJsonIncludesSubmissionPlan)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    AddStub(graph, "GfxFinal");
    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_phase_g9_submission_plan.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"submissionCommandCount\":"), std::string::npos);
    EXPECT_NE(json.find("\"submissionPlan\""), std::string::npos);
    EXPECT_NE(json.find("\"kind\": \"BatchBegin\""), std::string::npos);
    EXPECT_NE(json.find("\"kind\": \"BatchEnd\""), std::string::npos);
    EXPECT_NE(json.find("\"lane\": \"Compute\""), std::string::npos)
        << "Submission plan must serialise compute-lane assignment";
    EXPECT_NE(json.find("\"lane\": \"Graphics\""), std::string::npos)
        << "Submission plan must serialise graphics-lane assignment";
    EXPECT_NE(json.find("submissionCommands="), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraphSubmissionPlan, MultipleComputePassesSameIndexGetOneBatchPair)
{
    // Graph: C1 → C2 (consecutive compute) → GfxFinal.
    // The two passes form one batch → exactly one BatchBegin and one BatchEnd.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto c1 = AddStub(graph, "C1");
    c1->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    c1->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    auto c2 = AddStub(graph, "C2");
    c2->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);
    c2->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);

    AddStub(graph, "GfxFinal");
    graph.ConnectPass("C1", "C2");
    graph.ConnectPass("C2", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();

    EXPECT_EQ(CountKind(plan, SCKind::BatchBegin), 1u)
        << "Two consecutive compute passes → one batch open";
    EXPECT_EQ(CountKind(plan, SCKind::BatchEnd), 1u)
        << "Two consecutive compute passes → one batch close";
    EXPECT_EQ(CountKind(plan, SCKind::Pass), 3u);
}

TEST(RenderGraphSubmissionPlan, PlanPreservesHoistedExecutionOrder)
{
    // The Pass commands in the plan must appear in the same order as
    // graph.GetExecutionOrder() (which reflects HoistComputePasses()).
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();
    const auto planPassOrder = PassOrder(plan);
    const auto& graphPassOrder = graph.GetExecutionOrder();

    ASSERT_EQ(planPassOrder.size(), graphPassOrder.size());
    for (u32 i = 0; i < static_cast<u32>(planPassOrder.size()); ++i)
        EXPECT_EQ(planPassOrder[i], graphPassOrder[i])
            << "Plan pass order must match hoisted graph pass order at index " << i;
}

// =============================================================================
// RenderGraphExecutePlanDriven — Phase G Slice 6: Execute() via submission-plan IR
// =============================================================================

TEST(RenderGraphExecutePlanDriven, PureGraphicsGraphPassesExecuteInOrder)
{
    // A → B → C.  All three passes must execute exactly once and in
    // topological order.  No batch events must fire.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto a = AddStub(graph, "A");
    auto b = AddStub(graph, "B");
    auto c = AddStub(graph, "C");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C");
    graph.SetFinalPass("C");

    u32 batchEventCount = 0;
    graph.SetBatchEventHook([&](u32 /*batchIndex*/, bool /*isBegin*/)
                            { ++batchEventCount; });

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(c->GetExecuteCount(), 1u);
    EXPECT_EQ(batchEventCount, 0u) << "No async batches — no batch events expected";

    const auto& timings = graph.GetLastExecutionTimings();
    ASSERT_EQ(timings.size(), 3u);
    EXPECT_EQ(timings[0].NodeName, "A");
    EXPECT_EQ(timings[1].NodeName, "B");
    EXPECT_EQ(timings[2].NodeName, "C");
}

TEST(RenderGraphExecutePlanDriven, CulledPassIsSkippedInPlanDrivenExecution)
{
    // Isolated (no edges, no NeverCull) must be culled and must not execute.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto final = AddStub(graph, "Final");
    auto isolated = AddStub(graph, "Isolated");

    graph.SetFinalPass("Final");
    // Isolated has no NeverCull side effect and no path to Final.
    static_cast<void>(isolated);

    graph.Execute();

    EXPECT_EQ(final->GetExecuteCount(), 1u);
    EXPECT_EQ(isolated->GetExecuteCount(), 0u) << "Culled pass must not execute";
}

TEST(RenderGraphExecutePlanDriven, BatchEventHookFiresBeginAndEndForAsyncComputePass)
{
    // Graph: ComputePass (async candidate) → GfxFinal.
    // Execute() must fire exactly one Begin event and one End event.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");

    std::vector<std::pair<u32, bool>> events; // {batchIndex, isBegin}
    graph.SetBatchEventHook([&](u32 batchIndex, bool isBegin)
                            { events.emplace_back(batchIndex, isBegin); });

    graph.Execute();

    ASSERT_EQ(events.size(), 2u) << "Expected one Begin and one End event";
    EXPECT_TRUE(events[0].second) << "First event must be Begin";
    EXPECT_FALSE(events[1].second) << "Second event must be End";
}

TEST(RenderGraphExecutePlanDriven, BatchEventHookBatchIndexIsZeroForFirstBatch)
{
    // The first (and only) async batch must carry batch index 0.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");

    std::vector<u32> batchIndices;
    graph.SetBatchEventHook([&](u32 batchIndex, bool /*isBegin*/)
                            { batchIndices.push_back(batchIndex); });

    graph.Execute();

    ASSERT_EQ(batchIndices.size(), 2u);
    EXPECT_EQ(batchIndices[0], 0u) << "BatchBegin must carry index 0";
    EXPECT_EQ(batchIndices[1], 0u) << "BatchEnd must carry index 0";
}

TEST(RenderGraphExecutePlanDriven, PostPassHookStillFiresForEachPass)
{
    // SetPostPassHook contract: the hook must fire once per executed pass
    // in order, regardless of whether the plan-driven path is active.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "X");
    AddStub(graph, "Y");
    graph.ConnectPass("X", "Y");
    graph.SetFinalPass("Y");

    std::vector<std::string> firedFor;
    graph.SetPostPassHook([&](const std::string& passName, RenderGraph& /*g*/)
                          { firedFor.push_back(passName); });

    graph.Execute();

    ASSERT_EQ(firedFor.size(), 2u);
    EXPECT_EQ(firedFor[0], "X");
    EXPECT_EQ(firedFor[1], "Y");
}

// =============================================================================
// RenderGraphTemporalHistoryContracts — Phase G Slice 7: explicit temporal contracts
// =============================================================================

TEST(RenderGraphTemporalHistoryContracts, ExtractHistoryTextureRecordsContractAndInvokesCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto history = graph.ImportHistory(
        "TAAHistory",
        77,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "TAAHistory"));
    ASSERT_TRUE(history.IsValid());

    AddSetupNode(
        graph,
        "CurrentFrameProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                41,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("CurrentFrameProducer");
    graph.BuildFrameGraph();

    const auto sourceHandle = graph.GetTextureHandle("CurrentFrameColor");
    ASSERT_TRUE(sourceHandle.IsValid());

    bool callbackCalled = false;
    u32 extractedTextureID = 0;
    graph.ExtractHistoryTexture(
        "TAAHistory",
        sourceHandle,
        [&callbackCalled, &extractedTextureID](const u32 textureID)
        {
            callbackCalled = true;
            extractedTextureID = textureID;
        });

    graph.Execute();

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(extractedTextureID, 41u);

    const auto& contracts = graph.GetTemporalHistoryContracts();
    ASSERT_EQ(contracts.size(), 1u);
    EXPECT_EQ(contracts[0].HistoryResource, "TAAHistory");
    EXPECT_EQ(contracts[0].SourceResource, "CurrentFrameColor");
    EXPECT_TRUE(contracts[0].HistoryImported);
    EXPECT_TRUE(contracts[0].SourceReachable);
}

TEST(RenderGraphTemporalHistoryContracts, BuilderDeclaredHistoryExtractionRootsProducerAndDeduplicatesRuntimeContract)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto history = graph.ImportHistory(
        "TemporalHistory",
        77,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "TemporalHistory"));
    ASSERT_TRUE(history.IsValid());

    AddSetupNode(
        graph,
        "HistoryProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                41,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
            builder.ExtractHistoryTexture("TemporalHistory", color);
        });

    AddSetupNode(
        graph,
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                12,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(finalColor, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("FinalConsumer");
    graph.BuildFrameGraph();

    const auto& culledPasses = graph.GetCulledPasses();
    const auto culledIt = std::ranges::find(culledPasses, "HistoryProducer");
    EXPECT_EQ(culledIt, culledPasses.end()) << "Builder-declared history extraction should root producer reachability";

    const auto sourceHandle = graph.GetTextureHandle("CurrentFrameColor");
    ASSERT_TRUE(sourceHandle.IsValid());

    const auto& buildContracts = graph.GetTemporalHistoryContracts();
    ASSERT_EQ(buildContracts.size(), 1u);
    EXPECT_EQ(buildContracts[0].HistoryResource, "TemporalHistory");
    EXPECT_EQ(buildContracts[0].SourceResource, "CurrentFrameColor");
    EXPECT_TRUE(buildContracts[0].HistoryImported);
    EXPECT_TRUE(buildContracts[0].SourceReachable);

    bool callbackCalled = false;
    u32 extractedTextureID = 0;
    graph.ExtractHistoryTexture(
        "TemporalHistory",
        sourceHandle,
        [&callbackCalled, &extractedTextureID](const u32 textureID)
        {
            callbackCalled = true;
            extractedTextureID = textureID;
        });

    graph.Execute();

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(extractedTextureID, 41u);

    const auto& executeContracts = graph.GetTemporalHistoryContracts();
    ASSERT_EQ(executeContracts.size(), 1u);
    EXPECT_EQ(executeContracts[0].HistoryResource, "TemporalHistory");
    EXPECT_EQ(executeContracts[0].SourceResource, "CurrentFrameColor");
    EXPECT_TRUE(executeContracts[0].HistoryImported);
    EXPECT_TRUE(executeContracts[0].SourceReachable);
}

TEST(RenderGraphTemporalHistoryContracts, RegisteredHistorySinkCountsAsImportedAndInvalidatesWithoutNewCopy)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    bool historyValid = true;
    graph.RegisterHistoryTextureSink("TemporalHistory", 0, 0, 0, &historyValid);

    AddSetupNode(
        graph,
        "HistoryProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                41,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
            builder.ExtractHistoryTexture("TemporalHistory", color);
        });

    graph.SetFinalPass("HistoryProducer");
    graph.BuildFrameGraph();

    const auto& contracts = graph.GetTemporalHistoryContracts();
    ASSERT_EQ(contracts.size(), 1u);
    EXPECT_EQ(contracts[0].HistoryResource, "TemporalHistory");
    EXPECT_EQ(contracts[0].SourceResource, "CurrentFrameColor");
    EXPECT_TRUE(contracts[0].HistoryImported);
    EXPECT_TRUE(contracts[0].SourceReachable);

    graph.Execute();

    EXPECT_FALSE(historyValid);
}

TEST(RenderGraphTemporalHistoryContracts, InvalidHistoryContractReportsDiagnosticAndSkipsCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "CurrentFrameProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                51,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("CurrentFrameProducer");
    graph.BuildFrameGraph();

    const auto sourceHandle = graph.GetTextureHandle("CurrentFrameColor");
    ASSERT_TRUE(sourceHandle.IsValid());

    bool callbackCalled = false;
    graph.ExtractHistoryTexture(
        "MissingHistory",
        sourceHandle,
        [&callbackCalled](const u32 /*textureID*/)
        {
            callbackCalled = true;
        });

    graph.Execute();

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::ranges::find_if(diagnostics,
                                             [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                             {
                                                 return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::InvalidHistoryContract &&
                                                        diagnostic.Resource == "MissingHistory";
                                             });

    ASSERT_NE(diagIt, diagnostics.end());
    EXPECT_FALSE(callbackCalled);
}

TEST(RenderGraphTemporalHistoryContracts, DumpToJsonIncludesHistoryResourcesAndContracts)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto history = graph.ImportHistory(
        "TAAHistory",
        88,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "TAAHistory"));
    ASSERT_TRUE(history.IsValid());

    AddSetupNode(
        graph,
        "CurrentFrameProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                61,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("CurrentFrameProducer");
    graph.BuildFrameGraph();

    const auto sourceHandle = graph.GetTextureHandle("CurrentFrameColor");
    ASSERT_TRUE(sourceHandle.IsValid());

    graph.ExtractHistoryTexture(
        "TAAHistory",
        sourceHandle,
        [](const u32 /*textureID*/) {});

    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_phase_g7_history.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"historyResourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"temporalHistoryContractCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"TAAHistory\", \"kind\": \"Texture2D\", \"imported\": true, \"isHistory\": true"), std::string::npos);
    EXPECT_NE(json.find("\"historyResource\": \"TAAHistory\""), std::string::npos);
    EXPECT_NE(json.find("\"sourceResource\": \"CurrentFrameColor\""), std::string::npos);
    EXPECT_NE(json.find("\"historyImported\": true"), std::string::npos);
    EXPECT_NE(json.find("\"sourceReachable\": true"), std::string::npos);
    EXPECT_NE(json.find("histories=1"), std::string::npos);
    EXPECT_NE(json.find("historyContracts=1"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

TEST(RenderGraphTemporalHistoryContracts, ExtractHistoryTextureFromFramebufferAttachmentRecordsContractAndInvokesCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto history = graph.ImportHistory(
        "FogHistory",
        91,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "FogHistory"));
    ASSERT_TRUE(history.IsValid());

    auto currentFrameFramebuffer = Ref<AttachmentStubFramebuffer>(new AttachmentStubFramebuffer(17, 123));
    AddSetupNode(
        graph,
        "CurrentFrameProducer",
        [currentFrameFramebuffer](RGBuilder& builder)
        {
            auto color = builder.ImportFramebuffer(
                "CurrentFrameFogHalfRes",
                currentFrameFramebuffer,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "CurrentFrameFogHalfRes"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("CurrentFrameProducer");
    graph.BuildFrameGraph();

    const auto sourceHandle = graph.GetFramebufferHandle("CurrentFrameFogHalfRes");
    ASSERT_TRUE(sourceHandle.IsValid());

    bool callbackCalled = false;
    u32 extractedTextureID = 0;
    graph.ExtractHistoryTexture(
        "FogHistory",
        sourceHandle,
        [&callbackCalled, &extractedTextureID](const u32 textureID)
        {
            callbackCalled = true;
            extractedTextureID = textureID;
        });

    graph.Execute();

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(extractedTextureID, 123u);

    const auto& contracts = graph.GetTemporalHistoryContracts();
    ASSERT_EQ(contracts.size(), 1u);
    EXPECT_EQ(contracts[0].HistoryResource, "FogHistory");
    EXPECT_EQ(contracts[0].SourceResource, "CurrentFrameFogHalfRes");
    EXPECT_TRUE(contracts[0].HistoryImported);
    EXPECT_TRUE(contracts[0].SourceReachable);
}

// =============================================================================
// RenderGraphAsyncBatchResources — Phase G Slice 8: cross-batch resource deps
// =============================================================================

TEST(RenderGraphAsyncBatchResources, NoBatchResourceDepsWhenNoAccessDeclarations)
{
    // Stub passes have no registered access declarations, so InputResources
    // and OutputResources must both be empty even when WaitNodes/SignalNodes
    // are populated.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderGraphNode::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderGraphNode::SideEffect::NeverCull);
    AddStub(graph, "GfxPost");

    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_TRUE(batch.InputResources.empty())
        << "No access declarations → no InputResources expected";
    EXPECT_TRUE(batch.OutputResources.empty())
        << "No access declarations → no OutputResources expected";
}

TEST(RenderGraphAsyncBatchResources, SingleResourceFlowsIntoBatch)
{
    // Graph: GfxPre writes "SharedTex"; ComputeBatch reads "SharedTex".
    // Expected: one InputResource {SharedTex, GfxPre}.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SharedTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "ComputePass",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SharedTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    AddStub(graph, "GfxPost");
    graph.ConnectPass("GfxPre", "ComputePass");
    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    ASSERT_EQ(batch.InputResources.size(), 1u)
        << "SharedTex is written by GfxPre (external) and read by ComputePass → 1 InputResource";
    EXPECT_EQ(batch.InputResources[0].ResourceName, "SharedTex");
    EXPECT_EQ(batch.InputResources[0].ExternalNode, "GfxPre");

    EXPECT_TRUE(batch.OutputResources.empty())
        << "ComputePass does not write SharedTex → no OutputResources";
}

TEST(RenderGraphAsyncBatchResources, BatchOutputFlowsToGraphicsPass)
{
    // Graph: ComputeBatch writes "ResultTex"; GfxPost reads "ResultTex".
    // Expected: one OutputResource {ResultTex, GfxPost}.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ComputePass",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata |
            RenderGraphNodeFlags::NeverCull,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ResultTex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ResultTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_TRUE(batch.InputResources.empty())
        << "No external pass writes ResultTex before the batch → no InputResources";

    ASSERT_EQ(batch.OutputResources.size(), 1u)
        << "ResultTex is written by ComputePass and read by GfxPost (external) → 1 OutputResource";
    EXPECT_EQ(batch.OutputResources[0].ResourceName, "ResultTex");
    EXPECT_EQ(batch.OutputResources[0].ExternalNode, "GfxPost");
}

TEST(RenderGraphAsyncBatchResources, IndependentBatchHasNoCrossBoundaryResources)
{
    // ComputePass has its own private resource not shared with any other pass.
    // InputResources and OutputResources must both be empty.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ComputePass",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata |
            RenderGraphNodeFlags::NeverCull,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "PrivateTex",
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "PrivateTex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        });

    AddStub(graph, "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_TRUE(batch.InputResources.empty());
    EXPECT_TRUE(batch.OutputResources.empty());
}

TEST(RenderGraphAsyncBatchResources, DumpToJsonIncludesBatchResourceDeps)
{
    // Graph: GfxPre → ComputePass → GfxPost with a shared resource on each boundary.
    // The JSON asyncBatches array must contain the correct resource dep entries.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "InTex",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "InTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "ComputePass",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& builder)
        {
            auto inTex = builder.ImportTexture(
                "InTex",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "InTex"));
            [[maybe_unused]] const auto readInTex = builder.Read(inTex, RGReadUsage::ShaderSample);

            auto outTex = builder.ImportTexture(
                "OutTex",
                11,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "OutTex"));
            builder.Write(outTex, RGWriteUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto outTex = builder.ImportTexture(
                "OutTex",
                11,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "OutTex"));
            [[maybe_unused]] const auto readOutTex = builder.Read(outTex, RGReadUsage::ShaderSample);
        });

    graph.ConnectPass("GfxPre", "ComputePass");
    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath =
        std::filesystem::temp_directory_path() / "render_graph_phase_g8_batch_deps.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"asyncBatchCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"batchInputResourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"batchOutputResourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"asyncBatches\""), std::string::npos);
    EXPECT_NE(json.find("\"resource\": \"InTex\", \"externalPass\": \"GfxPre\""), std::string::npos)
        << "InTex must appear as an input resource produced by GfxPre";
    EXPECT_NE(json.find("\"resource\": \"OutTex\", \"externalPass\": \"GfxPost\""), std::string::npos)
        << "OutTex must appear as an output resource consumed by GfxPost";
    EXPECT_NE(json.find("batches=1;batchInputResources=1;batchOutputResources=1"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// ===========================================================================
// Phase G Slice 10 — Explicit resource transition records
// ===========================================================================

TEST(RenderGraphResourceTransitions, NoTransitionsWhenNoBarriersPlanned)
{
    // A graph with a single pass that has no declared reads/writes produces
    // no planned barriers and therefore no transition records.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "OnlyPass",
        [](RGBuilder& /*builder*/) {});
    graph.SetFinalPass("OnlyPass");
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    EXPECT_TRUE(transitions.empty())
        << "A graph with no declared access yields no transitions";
}

TEST(RenderGraphResourceTransitions, SingleTransitionCapturesProducerAndConsumer)
{
    // Producer writes "ColorTex"; Consumer reads "ColorTex".
    // After Execute() there must be exactly one transition record for
    // ColorTex with ProducerPass="Producer", ConsumerPass="Consumer",
    // FromUsage=RenderTarget, ToUsage=ShaderSample.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Producer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "ColorTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Consumer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "ColorTex"));
            [[maybe_unused]] const auto sampledTex = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Producer", "Consumer");
    graph.SetFinalPass("Consumer");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    ASSERT_EQ(transitions.size(), 1u);

    const auto& tr = transitions[0];
    EXPECT_EQ(tr.ResourceName, "ColorTex");
    EXPECT_EQ(tr.ProducerPass, "Producer");
    EXPECT_EQ(tr.ConsumerPass, "Consumer");
    EXPECT_EQ(tr.FromUsage, RGWriteUsage::RenderTarget);
    EXPECT_EQ(tr.ToUsage, RGReadUsage::ShaderSample);
    EXPECT_NE(tr.Flags, MemoryBarrierFlags::None)
        << "Barrier flags must be non-zero for a RenderTarget->ShaderSample transition";
}

TEST(RenderGraphResourceTransitions, ProducerIsLastWriterBeforeConsumer)
{
    // Two writers in order: Writer1 then Writer2. Consumer reads after both.
    // The transition for Tex->Consumer should come from Writer2 (last writer).
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Writer1",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Tex",
                5,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "Tex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        });

    AddSetupNode(
        graph,
        "Writer2",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Tex",
                5,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "Tex"));
            builder.Write(tex, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "Consumer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Tex",
                5,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "Tex"));
            [[maybe_unused]] const auto sampledTex = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("Writer1", "Writer2");
    graph.AddExecutionDependency("Writer2", "Consumer");
    graph.SetFinalPass("Consumer");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(transitions,
                                         [](const RenderGraph::ResourceTransition& t)
                                         { return t.ResourceName == "Tex" && t.ConsumerPass == "Consumer"; });
    ASSERT_NE(it, transitions.end()) << "Expected a transition record for Tex->Consumer";
    EXPECT_EQ(it->ProducerPass, "Writer2")
        << "Producer must be the LAST writer before the consumer";
}

TEST(RenderGraphResourceTransitions, ExternalImportHasNoProducerPass)
{
    // Resource imported at graph level (no pass writes it); a consumer reads it.
    // If a transition is created for it, ProducerPass must be "external".
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    [[maybe_unused]] const auto shadowHandle = graph.ImportTexture(
        "ShadowMap",
        42,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ShadowMap"));

    AddSetupNode(
        graph,
        "ShadowConsumer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ShadowMap",
                42,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ShadowMap"));
            [[maybe_unused]] const auto sampledShadow = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("ShadowConsumer");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    for (const auto& tr : transitions)
    {
        if (tr.ResourceName == "ShadowMap")
        {
            EXPECT_EQ(tr.ProducerPass, "external")
                << "Imported resource has no pass producer";
        }
    }
}

TEST(RenderGraphResourceTransitions, DumpToJsonIncludesResourceTransitions)
{
    // A graph with one write->read pair must have a non-empty
    // resourceTransitions array in the schema-9 JSON dump.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "PostPass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto sampledScene = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("ScenePass", "PostPass");
    graph.SetFinalPass("PostPass");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath =
        std::filesystem::temp_directory_path() / "render_graph_phase_g10_transitions.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos)
        << "Schema must be version 16 after external-backing dump visibility updates";
    EXPECT_NE(json.find("\"resourceTransitionCount\": 1"), std::string::npos)
        << "frameSummary must expose resourceTransitionCount";
    EXPECT_NE(json.find("\"resourceTransitions\""), std::string::npos)
        << "resourceTransitions array must be present";
    EXPECT_NE(json.find("\"producerPass\": \"ScenePass\""), std::string::npos)
        << "Transition record must name the producer pass";
    EXPECT_NE(json.find("\"consumerPass\": \"PostPass\""), std::string::npos)
        << "Transition record must name the consumer pass";
    EXPECT_NE(json.find("\"fromUsage\": \"RenderTarget\""), std::string::npos)
        << "From-usage must be serialised as RenderTarget";
    EXPECT_NE(json.find("\"toUsage\": \"ShaderSample\""), std::string::npos)
        << "To-usage must be serialised as ShaderSample";
    EXPECT_NE(json.find(";transitions=1"), std::string::npos)
        << "graphDigest must contain the transition count";

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// ============================================================================
// Phase G Slice 11 — Unified resource lifetime records (GetResourceLifetimes)
// ============================================================================

TEST(RenderGraphResourceLifetimes, NoLifetimesWhenNoResourcesDeclared)
{
    // An empty graph (no passes, no resources) must return an empty vector.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    EXPECT_TRUE(graph.GetResourceLifetimes().empty());
}

TEST(RenderGraphResourceLifetimes, TransientResourceHasCorrectFirstAndLastPass)
{
    // A resource written in PassA and read in PassB must have
    //   FirstWritePass == "PassA"  (index 0)
    //   LastReadPass  == "PassB"  (index 1)
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "PassA",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Color",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "Color"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "PassB",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Color",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "Color"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("PassA", "PassB");
    graph.SetFinalPass("PassB");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto lifetimes = graph.GetResourceLifetimes();
    ASSERT_FALSE(lifetimes.empty());

    const auto it = std::ranges::find_if(lifetimes,
                                         [](const RenderGraph::ResourceLifetime& lt)
                                         { return lt.ResourceName == "Color"; });
    ASSERT_NE(it, lifetimes.end()) << "Color resource must appear in lifetimes";

    EXPECT_EQ(it->FirstWritePass, "PassA");
    EXPECT_EQ(it->LastReadPass, "PassB");
    EXPECT_LT(it->FirstWritePassIndex, std::numeric_limits<u32>::max());
    EXPECT_LT(it->LastReadPassIndex, std::numeric_limits<u32>::max());
    EXPECT_LT(it->FirstWritePassIndex, it->LastReadPassIndex);
    EXPECT_EQ(it->FirstWriteUsage, RGWriteUsage::RenderTarget);
    EXPECT_EQ(it->LastReadUsage, RGReadUsage::ShaderSample);
}

TEST(RenderGraphResourceLifetimes, ImportedResourceHasExternalFirstWrite)
{
    // A resource that is imported but never written by any pass must have
    // FirstWritePass == "external" and FirstWritePassIndex == UINT32_MAX.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ConsumerPass",
        [](RGBuilder& builder)
        {
            // Only read — never write — so no pass acts as producer.
            auto tex = builder.ImportTexture(
                "ExtTex",
                99,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ExtTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("ConsumerPass");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto lifetimes = graph.GetResourceLifetimes();
    const auto it = std::ranges::find_if(lifetimes,
                                         [](const RenderGraph::ResourceLifetime& lt)
                                         { return lt.ResourceName == "ExtTex"; });
    ASSERT_NE(it, lifetimes.end());

    EXPECT_EQ(it->FirstWritePass, "external")
        << "Import-only resource must report FirstWritePass as external";
    EXPECT_EQ(it->FirstWritePassIndex, std::numeric_limits<u32>::max())
        << "Import-only resource must have UINT32_MAX FirstWritePassIndex";
    EXPECT_TRUE(it->IsImported) << "Resource imported via ImportTexture must have IsImported==true";
}

TEST(RenderGraphResourceLifetimes, WriteOnlyResourceHasEmptyLastRead)
{
    // A resource that is written but never read must have
    // LastReadPass == "" and LastReadPassIndex == UINT32_MAX.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "WriterPass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SinkTex",
                7,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SinkTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("WriterPass");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto lifetimes = graph.GetResourceLifetimes();
    const auto it = std::ranges::find_if(lifetimes,
                                         [](const RenderGraph::ResourceLifetime& lt)
                                         { return lt.ResourceName == "SinkTex"; });
    ASSERT_NE(it, lifetimes.end());

    EXPECT_NE(it->FirstWritePass, "external")
        << "Written resource must not be external";
    EXPECT_EQ(it->LastReadPass, "")
        << "Write-only resource must have empty LastReadPass";
    EXPECT_EQ(it->LastReadPassIndex, std::numeric_limits<u32>::max())
        << "Write-only resource must have UINT32_MAX LastReadPassIndex";
}

TEST(RenderGraphResourceLifetimes, DumpToJsonIncludesResourceLifetimes)
{
    // A graph with one write->read pair must have a non-empty
    // resourceLifetimes array in the schema-10 JSON dump.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "PostPass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("ScenePass", "PostPass");
    graph.SetFinalPass("PostPass");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath =
        std::filesystem::temp_directory_path() / "render_graph_phase_g11_lifetimes.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos)
        << "Schema must be version 16 after external-backing dump visibility updates";
    EXPECT_NE(json.find("\"resourceLifetimeCount\""), std::string::npos)
        << "frameSummary must expose resourceLifetimeCount";
    EXPECT_NE(json.find("\"resourceLifetimes\""), std::string::npos)
        << "resourceLifetimes array must be present";
    EXPECT_NE(json.find("\"hasExternalBacking\": false"), std::string::npos)
        << "Resource lifetime dump must include hasExternalBacking";
    EXPECT_NE(json.find("\"firstWritePass\": \"ScenePass\""), std::string::npos)
        << "Lifetime record must name the first-write pass";
    EXPECT_NE(json.find("\"lastReadPass\": \"PostPass\""), std::string::npos)
        << "Lifetime record must name the last-read pass";
    EXPECT_NE(json.find("\"firstWriteUsage\": \"RenderTarget\""), std::string::npos)
        << "First-write usage must be serialised";
    EXPECT_NE(json.find("\"lastReadUsage\": \"ShaderSample\""), std::string::npos)
        << "Last-read usage must be serialised";
    EXPECT_NE(json.find(";lifetimes="), std::string::npos)
        << "graphDigest must contain the lifetime count";

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// ---------------------------------------------------------------------------
// Phase G Slice 12: RGSubresourceRange propagation into barriers/transitions
// ---------------------------------------------------------------------------

TEST(RenderGraphSubresourceRange, FullRangeByDefaultWhenNoRangeSpecified)
{
    // A plain Read() call with no explicit range should produce a transition
    // and barrier with the default "full range" (all fields == ~0u for counts,
    // BaseMip / BaseLayer / BaseSlice == 0).
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "Writer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ColorTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Reader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ColorTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.ConnectPass("Writer", "Reader");
    graph.SetFinalPass("Reader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(transitions,
                                         [](const RenderGraph::ResourceTransition& t)
                                         { return t.ResourceName == "ColorTex"; });
    ASSERT_NE(it, transitions.end()) << "Expected a transition record for ColorTex";

    const auto& range = it->Range;
    EXPECT_EQ(range.BaseMip, 0u) << "Default BaseMip must be 0";
    EXPECT_EQ(range.MipCount, ~0u) << "Default MipCount must be ~0u (all mips)";
    EXPECT_EQ(range.BaseLayer, 0u) << "Default BaseLayer must be 0";
    EXPECT_EQ(range.LayerCount, ~0u) << "Default LayerCount must be ~0u (all layers)";
}

TEST(RenderGraphSubresourceRange, MipRangePreservedInTransition)
{
    // A Read() call with RGSubresourceRange::Mip(2) must propagate
    // BaseMip==2 and MipCount==1 into the ResourceTransition.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "MipWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "MipTex",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "MipTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "MipReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "MipTex",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "MipTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(2));
        });

    graph.ConnectPass("MipWriter", "MipReader");
    graph.SetFinalPass("MipReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(transitions,
                                         [](const RenderGraph::ResourceTransition& t)
                                         { return t.ResourceName == "MipTex"; });
    ASSERT_NE(it, transitions.end()) << "Expected a transition record for MipTex";

    EXPECT_EQ(it->Range.BaseMip, 2u) << "BaseMip must match the declared mip";
    EXPECT_EQ(it->Range.MipCount, 1u) << "MipCount must be 1 for Mip(2)";
    EXPECT_EQ(it->Range.BaseLayer, 0u) << "BaseLayer must default to 0";
}

TEST(RenderGraphSubresourceRange, LayerRangePreservedInTransition)
{
    // A Read() call with RGSubresourceRange::Layer(3) must propagate
    // BaseLayer==3 and LayerCount==1 into the ResourceTransition.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "LayerWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "LayerTex",
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "LayerTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "LayerReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "LayerTex",
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "LayerTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Layer(3));
        });

    graph.ConnectPass("LayerWriter", "LayerReader");
    graph.SetFinalPass("LayerReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(transitions,
                                         [](const RenderGraph::ResourceTransition& t)
                                         { return t.ResourceName == "LayerTex"; });
    ASSERT_NE(it, transitions.end()) << "Expected a transition record for LayerTex";

    EXPECT_EQ(it->Range.BaseLayer, 3u) << "BaseLayer must match the declared layer";
    EXPECT_EQ(it->Range.LayerCount, 1u) << "LayerCount must be 1 for Layer(3)";
    EXPECT_EQ(it->Range.BaseMip, 0u) << "BaseMip must default to 0";
}

TEST(RenderGraphSubresourceRange, MultiLayerDeclarationsProducePerLayerTransitions)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ShadowWriter",
        [](RGBuilder& builder)
        {
            auto csm = builder.ImportTexture(
                "ShadowCSM",
                13,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, "ShadowCSM"));
            for (u32 cascade = 0; cascade < 4u; ++cascade)
            {
                builder.Write(csm, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(cascade));
            }
        });

    AddSetupNode(
        graph,
        "ShadowReader",
        [](RGBuilder& builder)
        {
            auto csm = builder.ImportTexture(
                "ShadowCSM",
                13,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2DArray, "ShadowCSM"));
            for (u32 cascade = 0; cascade < 4u; ++cascade)
            {
                [[maybe_unused]] const auto r =
                    builder.Read(csm, RGReadUsage::ShaderSample, RGSubresourceRange::Layer(cascade));
            }
        });

    graph.ConnectPass("ShadowWriter", "ShadowReader");
    graph.SetFinalPass("ShadowReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    for (u32 cascade = 0; cascade < 4u; ++cascade)
    {
        const auto layerIt = std::ranges::find_if(
            transitions,
            [cascade](const RenderGraph::ResourceTransition& transition)
            {
                return transition.ResourceName == "ShadowCSM" &&
                       transition.Range.BaseLayer == cascade &&
                       transition.Range.LayerCount == 1u;
            });
        ASSERT_NE(layerIt, transitions.end())
            << "Expected per-layer transition for cascade " << cascade;
    }
}

TEST(RenderGraphSubresourceRange, SliceRangePreservedInTransition)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "PointShadowWriter",
        [](RGBuilder& builder)
        {
            auto cubemap = builder.ImportTexture(
                "ShadowPoint0",
                21,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, "ShadowPoint0"));
            RGSubresourceRange faceRange{};
            faceRange.BaseSlice = 4u;
            faceRange.SliceCount = 1u;
            builder.Write(cubemap, RGWriteUsage::DepthStencil, faceRange);
        });

    AddSetupNode(
        graph,
        "PointShadowReader",
        [](RGBuilder& builder)
        {
            auto cubemap = builder.ImportTexture(
                "ShadowPoint0",
                21,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::TextureCube, "ShadowPoint0"));
            RGSubresourceRange faceRange{};
            faceRange.BaseSlice = 4u;
            faceRange.SliceCount = 1u;
            [[maybe_unused]] const auto r = builder.Read(cubemap, RGReadUsage::ShaderSample, faceRange);
        });

    graph.ConnectPass("PointShadowWriter", "PointShadowReader");
    graph.SetFinalPass("PointShadowReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(transitions,
                                         [](const RenderGraph::ResourceTransition& transition)
                                         {
                                             return transition.ResourceName == "ShadowPoint0";
                                         });
    ASSERT_NE(it, transitions.end()) << "Expected a transition record for ShadowPoint0";

    EXPECT_EQ(it->Range.BaseSlice, 4u);
    EXPECT_EQ(it->Range.SliceCount, 1u);
}

TEST(RenderGraphSubresourceRange, PlannedBarrierCarriesRange)
{
    // The PlannedBarrier for a Write->Read pair with an explicit subresource
    // range must expose that range via PlannedBarrier::Range.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "BarrierWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "BaTex",
                4,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "BaTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "BarrierReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "BaTex",
                4,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "BaTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(1));
        });

    graph.ConnectPass("BarrierWriter", "BarrierReader");
    graph.SetFinalPass("BarrierReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& barriers = graph.GetPlannedBarriers();
    const auto it = std::ranges::find_if(barriers,
                                         [](const RenderGraph::PlannedBarrier& b)
                                         { return b.Resource == "BaTex" && b.BeforePass == "BarrierReader"; });
    ASSERT_NE(it, barriers.end()) << "Expected a planned barrier for BaTex before BarrierReader";

    EXPECT_EQ(it->Range.BaseMip, 1u) << "PlannedBarrier.Range.BaseMip must match Read() declaration";
    EXPECT_EQ(it->Range.MipCount, 1u) << "PlannedBarrier.Range.MipCount must be 1 for Mip(1)";
}

TEST(RenderGraphSubresourceRange, DumpToJsonIncludesRange)
{
    // Schema 11 DumpToJson must include range objects in plannedBarriers and
    // resourceTransitions entries.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "JsonWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "JsonTex",
                5,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "JsonTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "JsonReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "JsonTex",
                5,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "JsonTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(0));
        });

    graph.ConnectPass("JsonWriter", "JsonReader");
    graph.SetFinalPass("JsonReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath =
        std::filesystem::temp_directory_path() / "rg_subresource_range_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open()) << "DumpToJson must create the output file";
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos)
        << "Schema must be version 16 after external-backing dump visibility updates";
    EXPECT_NE(json.find("\"range\""), std::string::npos)
        << "At least one range object must be present in the JSON output";
    EXPECT_NE(json.find("\"baseMip\""), std::string::npos)
        << "range objects must contain baseMip";
    EXPECT_NE(json.find("\"mipCount\""), std::string::npos)
        << "range objects must contain mipCount";
    EXPECT_NE(json.find("\"baseLayer\""), std::string::npos)
        << "range objects must contain baseLayer";
    EXPECT_NE(json.find("\"layerCount\""), std::string::npos)
        << "range objects must contain layerCount";
    EXPECT_NE(json.find(";subresourceRanges=present"), std::string::npos)
        << "graphDigest must flag subresource range presence";

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// ============================================================
// Phase G Slice 14 — Cross-lane sync metadata
// ============================================================

TEST(RenderGraphCrossLaneSync, PureGraphicsGraphHasNoCrossLaneTransitions)
{
    // A graph consisting solely of graphics passes must produce zero cross-lane
    // transitions: every IsCrossLane flag must be false.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "GfxTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "GfxTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "GfxReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "GfxTex",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "GfxTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("GfxWriter", "GfxReader");
    graph.SetFinalPass("GfxReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    for (const auto& tr : transitions)
    {
        EXPECT_FALSE(tr.IsCrossLane)
            << "Graphics-only graph must have no cross-lane transitions (resource='"
            << tr.ResourceName << "')";
        EXPECT_EQ(tr.ProducerLane, RenderGraph::QueueLane::Graphics);
        EXPECT_EQ(tr.ConsumerLane, RenderGraph::QueueLane::Graphics);
    }
}

TEST(RenderGraphCrossLaneSync, ComputeToGraphicsTransitionIsCrossLane)
{
    // A compute pass writes "ComputeResult"; a graphics pass reads it.
    // The transition must be flagged as cross-lane with ProducerLane=Compute
    // and ConsumerLane=Graphics.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ComputeWriter",
        RenderGraphNodeFlags::Compute,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ComputeResult",
                7,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ComputeResult"));
            builder.Write(tex, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "GfxReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ComputeResult",
                7,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "ComputeResult"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("ComputeWriter", "GfxReader");
    graph.SetFinalPass("GfxReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(transitions,
                                         [](const RenderGraph::ResourceTransition& t)
                                         {
                                             return t.ResourceName == "ComputeResult" &&
                                                    t.ConsumerPass == "GfxReader";
                                         });
    ASSERT_NE(it, transitions.end())
        << "Expected a transition record for ComputeResult -> GfxReader";

    EXPECT_TRUE(it->IsCrossLane)
        << "Compute->Graphics transition must be flagged IsCrossLane=true";
    EXPECT_EQ(it->ProducerLane, RenderGraph::QueueLane::Compute)
        << "ProducerLane must be Compute";
    EXPECT_EQ(it->ConsumerLane, RenderGraph::QueueLane::Graphics)
        << "ConsumerLane must be Graphics";
    EXPECT_EQ(it->ProducerPass, "ComputeWriter");
}

TEST(RenderGraphCrossLaneSync, DumpToJsonIncludesCrossLaneSyncFields)
{
    // Schema 12 DumpToJson must export isCrossLane, producerLane, consumerLane
    // fields on each resourceTransitions entry and crossLaneSyncCount in
    // frameSummary/graphDigest when a cross-lane transition is present.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "CsWriter",
        RenderGraphNodeFlags::Compute,
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "CsTex",
                9,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CsTex"));
            builder.Write(tex, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "GsReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "CsTex",
                9,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "CsTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("CsWriter", "GsReader");
    graph.SetFinalPass("GsReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto outputPath =
        std::filesystem::temp_directory_path() / "rg_cross_lane_sync_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open()) << "DumpToJson must create the output file";
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos)
        << "Schema must be version 16 after external-backing dump visibility updates";
    EXPECT_NE(json.find("\"crossLaneSyncCount\""), std::string::npos)
        << "frameSummary must include crossLaneSyncCount";
    EXPECT_NE(json.find("\"isCrossLane\""), std::string::npos)
        << "Each resourceTransitions entry must include isCrossLane";
    EXPECT_NE(json.find("\"producerLane\""), std::string::npos)
        << "Each resourceTransitions entry must include producerLane";
    EXPECT_NE(json.find("\"consumerLane\""), std::string::npos)
        << "Each resourceTransitions entry must include consumerLane";
    EXPECT_NE(json.find("\"isCrossLane\": true"), std::string::npos)
        << "At least one transition must be flagged as cross-lane";
    EXPECT_NE(json.find(";crossLaneSync="), std::string::npos)
        << "graphDigest must include crossLaneSync entry";

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// ============================================================
// Phase G Slice 15 — Queue-aware scheduler validation
// ============================================================

TEST(RenderGraphQueueAwareScheduler, LegalOverlapDisjointResourcesNoHazard)
{
    // A graphics pass writing SceneColor and a compute pass writing AO (a
    // completely different resource) must produce zero resource hazards when
    // a downstream graphics pass reads both. This models the legal GTAO
    // overlap pattern: SceneColor and AO are produced in parallel.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "ComputeAO",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "AO",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AO"));
            builder.Write(ao, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneColor"));
            [[maybe_unused]] const auto r1 = builder.Read(sc, RGReadUsage::ShaderSample);

            auto ao = builder.ImportTexture(
                "AO",
                2,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AO"));
            [[maybe_unused]] const auto r2 = builder.Read(ao, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("GfxPre", "GfxPost");
    graph.AddExecutionDependency("ComputeAO", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Disjoint-resource compute+graphics must report no resource hazards";
}

TEST(RenderGraphQueueAwareScheduler, ForbiddenOverlapComputeWritesAfterGraphicsRead)
{
    // A compute pass writes a resource that a prior graphics pass has already
    // read, with no execution dependency connecting the read to the write.
    // ValidateResourceHazards must detect a WriteAfterRead hazard.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxReader",
        RenderGraphNodeFlags::Graphics | RenderGraphNodeFlags::NeverCull,
        [](RGBuilder& builder)
        {
            auto depth = builder.ImportTexture(
                "SceneDepth",
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneDepth"));
            [[maybe_unused]] const auto r = builder.Read(depth, RGReadUsage::ShaderSample);
        });

    AddSetupNode(
        graph,
        "ComputeWriter",
        RenderGraphNodeFlags::Compute,
        [](RGBuilder& builder)
        {
            auto depth = builder.ImportTexture(
                "SceneDepth",
                3,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SceneDepth"));
            builder.Write(depth, RGWriteUsage::ShaderStorage);
        });

    // Intentionally NO execution dependency from GfxReader to ComputeWriter —
    // this models a programmer error that the hazard validator must catch.
    graph.SetFinalPass("ComputeWriter");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const bool hasWriteAfterRead = std::ranges::any_of(
        hazards,
        [](const RenderGraph::Hazard& h)
        {
            return h.Kind == RenderGraph::HazardKind::WriteAfterRead &&
                   h.Resource == "SceneDepth";
        });
    EXPECT_TRUE(hasWriteAfterRead)
        << "WriteAfterRead hazard must be detected when compute overwrites a "
           "resource that a prior graphics pass reads without an ordering edge";
}

TEST(RenderGraphQueueAwareScheduler, OrderingPreservedAfterComputeHoist)
{
    // Graph: GfxA → ComputeB → GfxC (strict sequential chain).
    // After hoisting, pass order must still be [GfxA, ComputeB, GfxC] —
    // the scheduler must not reorder passes that have declared ordering edges.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "GfxA",
        [](RGBuilder& /*builder*/) {});

    AddSetupNode(
        graph,
        "ComputeB",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& /*builder*/) {});

    AddSetupNode(
        graph,
        "GfxC",
        [](RGBuilder& /*builder*/) {});

    graph.AddExecutionDependency("GfxA", "ComputeB");
    graph.AddExecutionDependency("ComputeB", "GfxC");
    graph.SetFinalPass("GfxC");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), 3u);

    const auto posA = std::ranges::find(order, "GfxA") - order.begin();
    const auto posB = std::ranges::find(order, "ComputeB") - order.begin();
    const auto posC = std::ranges::find(order, "GfxC") - order.begin();

    EXPECT_LT(posA, posB) << "GfxA must come before ComputeB in the hoisted order";
    EXPECT_LT(posB, posC) << "ComputeB must come before GfxC in the hoisted order";
}

TEST(RenderGraphQueueAwareScheduler, GTAOStyleComputeToGraphicsCrossLaneTransition)
{
    // GTAO pattern: HZBPass (compute) writes AO; LightingPass (graphics) reads it.
    // Expected: exactly one transition for AO, flagged IsCrossLane=true,
    // ProducerLane=Compute, ConsumerLane=Graphics, and no hazards when properly ordered.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "HZBPass",
        RenderGraphNodeFlags::Compute,
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "AOTexture",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AOTexture"));
            builder.Write(ao, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "LightingPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "AOTexture",
                10,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "AOTexture"));
            [[maybe_unused]] const auto r = builder.Read(ao, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("HZBPass", "LightingPass");
    graph.SetFinalPass("LightingPass");
    graph.BuildFrameGraph();
    graph.Execute();

    // No hazards when ordering is declared.
    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "GTAO-style compute→graphics must be hazard-free when ordered";

    // The AO transition must be flagged as cross-lane.
    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::ranges::find_if(
        transitions,
        [](const RenderGraph::ResourceTransition& tr)
        {
            return tr.ResourceName == "AOTexture" && tr.ConsumerPass == "LightingPass";
        });
    ASSERT_NE(it, transitions.end())
        << "AOTexture → LightingPass transition must exist";
    EXPECT_TRUE(it->IsCrossLane)
        << "Compute→Graphics transition must be IsCrossLane=true";
    EXPECT_EQ(it->ProducerLane, RenderGraph::QueueLane::Compute);
    EXPECT_EQ(it->ConsumerLane, RenderGraph::QueueLane::Graphics);
}

TEST(RenderGraphQueueAwareScheduler, HazardValidatorRemainsGreenAfterComputeHoist)
{
    // A graph with two compute passes and one graphics consumer that
    // depends on both outputs. After hoisting, all ordering constraints
    // must still be satisfied and no hazards reported.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ComputeGTAO",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "GTAOResult",
                20,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "GTAOResult"));
            builder.Write(ao, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "ComputeSSGI",
        RenderGraphNodeFlags::Compute | RenderGraphNodeFlags::AsyncCandidateMetadata,
        [](RGBuilder& builder)
        {
            auto gi = builder.ImportTexture(
                "SSGIResult",
                21,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SSGIResult"));
            builder.Write(gi, RGWriteUsage::ShaderStorage);
        });

    AddSetupNode(
        graph,
        "DeferredLighting",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "GTAOResult",
                20,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "GTAOResult"));
            [[maybe_unused]] const auto r1 = builder.Read(ao, RGReadUsage::ShaderSample);

            auto gi = builder.ImportTexture(
                "SSGIResult",
                21,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "SSGIResult"));
            [[maybe_unused]] const auto r2 = builder.Read(gi, RGReadUsage::ShaderSample);
        });

    graph.AddExecutionDependency("ComputeGTAO", "DeferredLighting");
    graph.AddExecutionDependency("ComputeSSGI", "DeferredLighting");
    graph.SetFinalPass("DeferredLighting");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Hazard validator must remain green after compute hoist on multi-compute graph";

    const auto& order = graph.GetExecutionOrder();
    const auto posGTAO = std::ranges::find(order, "ComputeGTAO") - order.begin();
    const auto posSSGI = std::ranges::find(order, "ComputeSSGI") - order.begin();
    const auto posLighting =
        std::ranges::find(order, "DeferredLighting") - order.begin();

    EXPECT_LT(posGTAO, posLighting)
        << "ComputeGTAO must precede DeferredLighting after hoist";
    EXPECT_LT(posSSGI, posLighting)
        << "ComputeSSGI must precede DeferredLighting after hoist";
}

// ============================================================
// Phase H Slice 1 — resolve-failure telemetry contracts
// ============================================================

TEST(RenderGraphResolveFailureTelemetry, InvalidTypedHandleResolvesAreRecorded)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto probe = Ref<ResolveFailureProbePass>::Create("ProbePass");
    AddPassNode(graph, probe);
    graph.SetFinalPass("ProbePass");

    graph.Execute();

    const auto& failures = graph.GetResolveFailures();
    ASSERT_FALSE(failures.empty())
        << "Invalid typed-handle resolves should emit resolve-failure telemetry";

    const auto hasInvalidTexture = std::ranges::any_of(failures,
                                                       [](const RenderGraph::ResolveFailure& failure)
                                                       {
                                                           return failure.PassName == "ProbePass" &&
                                                                  failure.Reason == "invalid-texture-handle" &&
                                                                  failure.Count >= 1u;
                                                       });
    const auto hasInvalidFramebuffer = std::ranges::any_of(failures,
                                                           [](const RenderGraph::ResolveFailure& failure)
                                                           {
                                                               return failure.PassName == "ProbePass" &&
                                                                      failure.Reason == "invalid-framebuffer-handle" &&
                                                                      failure.Count >= 1u;
                                                           });

    EXPECT_TRUE(hasInvalidTexture);
    EXPECT_TRUE(hasInvalidFramebuffer);
    EXPECT_TRUE(probe->LastResolvedFramebufferWasNull());
}

TEST(RenderGraphResolveFailureTelemetry, ValidTextureResolveDoesNotEmitTextureFailure)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    const auto textureHandle = graph.ImportTexture(
        "TelemetryTex",
        123,
        RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, "TelemetryTex"));
    ASSERT_TRUE(textureHandle.IsValid());

    auto probe = Ref<ResolveFailureProbePass>::Create("ProbePass");
    probe->SetTextureHandle(textureHandle);
    AddPassNode(graph, probe);
    graph.SetFinalPass("ProbePass");

    graph.Execute();

    EXPECT_EQ(probe->GetLastResolvedTexture(), 123u);

    const auto& failures = graph.GetResolveFailures();
    const auto hasTextureFailure = std::ranges::any_of(failures,
                                                       [](const RenderGraph::ResolveFailure& failure)
                                                       {
                                                           return failure.PassName == "ProbePass" &&
                                                                  (failure.Reason == "invalid-texture-handle" ||
                                                                   failure.Reason == "stale-texture-handle" ||
                                                                   failure.Reason == "texture-resolve-zero");
                                                       });
    EXPECT_FALSE(hasTextureFailure)
        << "Valid texture resolve should not emit texture resolve-failure telemetry";
}

TEST(RenderGraphResolveFailureTelemetry, DumpToJsonUsesResolveFailureFieldNames)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto probe = Ref<ResolveFailureProbePass>::Create("ProbePass");
    AddPassNode(graph, probe);
    graph.SetFinalPass("ProbePass");

    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_resolve_failure_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());

    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string json = buffer.str();

    EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"resolveFailureCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"resolveFailures\": ["), std::string::npos);
    EXPECT_EQ(json.find("\"fallbackActivationCount\""), std::string::npos);
    EXPECT_EQ(json.find("\"fallbackActivations\""), std::string::npos);
}

// ============================================================
// SceneColor RMW chain via builder callbacks
// Verifies that node-owned read/write declarations drive the
// correct RAW-edge derivation in BuildFrameGraph instead of
// relying on write-after-write insertion order.
// ============================================================

TEST(RenderGraphSceneColorChain, RMWChainDrivesRAWEdgesViaBuilderCallbacks)
{
    // Forward-path SceneColor chain: ScenePass writes, then Foliage/Decal
    // each read-then-write (RMW). BuildFrameGraph must derive
    //   ScenePass → FoliagePass   (RAW: Foliage reads what Scene wrote)
    //   FoliagePass → DecalPass   (RAW: Decal reads what Foliage wrote)
    // without any explicit AddExecutionDependency call.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "FoliagePass",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto r = builder.Read(sc, RGReadUsage::RenderTargetRead);
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "DecalPass",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto r = builder.Read(sc, RGReadUsage::RenderTargetRead);
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("DecalPass");

    // No explicit AddExecutionDependency — ordering must come from Read+Write declarations.
    graph.BuildFrameGraph();

    // At least 2 derived edges must exist: ScenePass→FoliagePass and FoliagePass→DecalPass.
    const auto& stats = graph.GetLastBuildStats();
    EXPECT_GE(stats.DerivedEdges, 2u)
        << "BuildFrameGraph must derive at least 2 edges from the SceneColor RMW chain";

    // Execution order must reflect the derived RAW chain: ScenePass first, DecalPass last.
    const auto& order = graph.GetExecutionOrder();
    const auto scenePos = std::ranges::find(order, "ScenePass") - order.begin();
    const auto foliagePos = std::ranges::find(order, "FoliagePass") - order.begin();
    const auto decalPos = std::ranges::find(order, "DecalPass") - order.begin();

    EXPECT_LT(scenePos, foliagePos)
        << "ScenePass must execute before FoliagePass (derived from SceneColor Read on FoliagePass)";
    EXPECT_LT(foliagePos, decalPos)
        << "FoliagePass must execute before DecalPass (derived from SceneColor Read on DecalPass)";
}

TEST(RenderGraphSceneColorChain, DeferredSceneColorRMWChainViaBuilderCallbacksIsHazardFree)
{
    // Deferred-path chain:
    //   DeferredLightingPass writes SceneColor
    //   ForwardOverlayPass   reads + writes SceneColor (RMW)
    //   FoliagePass          reads + writes SceneColor (RMW)
    // BuildFrameGraph must derive the full ordering chain from builder
    // callbacks with zero explicit edges between these passes.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "DeferredLightingPass",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                2u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "ForwardOverlayPass",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                2u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto r = builder.Read(sc, RGReadUsage::RenderTargetRead);
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "FoliagePass",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                2u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto r = builder.Read(sc, RGReadUsage::RenderTargetRead);
            builder.Write(sc, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("FoliagePass");

    // No explicit ordering edges — builder callbacks must drive the full chain.
    graph.BuildFrameGraph();

    // Expect at least 2 derived edges: DeferredLighting→Overlay, Overlay→Foliage.
    const auto& stats = graph.GetLastBuildStats();
    EXPECT_GE(stats.DerivedEdges, 2u)
        << "BuildFrameGraph must derive at least 2 RAW edges for the deferred SceneColor chain";

    // Execution order must reflect the derived chain.
    const auto& order = graph.GetExecutionOrder();
    const auto lightingPos =
        std::ranges::find(order, "DeferredLightingPass") - order.begin();
    const auto overlayPos =
        std::ranges::find(order, "ForwardOverlayPass") - order.begin();
    const auto foliagePos =
        std::ranges::find(order, "FoliagePass") - order.begin();

    EXPECT_LT(lightingPos, overlayPos)
        << "DeferredLightingPass must precede ForwardOverlayPass (derived from SceneColor Read)";
    EXPECT_LT(overlayPos, foliagePos)
        << "ForwardOverlayPass must precede FoliagePass (derived from SceneColor Read)";
}

TEST(RenderGraphSceneColorChain, RmwPassRemainsReachableWhenAllOptionalRmwChainStepsAreAbsent)
{
    // Regression: a deferred RMW pass that does `WriteNewVersion(SceneColor, …)`
    // (e.g. ForwardOverlayPass) used to be culled with "No downstream reader"
    // whenever every optional pinning step between it and the post-process
    // chain (Foliage / Decal / Water / Particle / OITResolve) had empty
    // buckets and short-circuited their Setup. Post-process passes look up
    // `SceneColorTexture` via the latest-version map, but `WriteNewVersion`
    // only republished the framebuffer version — the texture-view name still
    // resolved to the base view (which is parented on the base framebuffer),
    // so the dep walked back to the base writer (DeferredLightingPass) and
    // skipped ForwardOverlay entirely.
    //
    // `CreateVersionedFramebufferHandle` now auto-publishes versioned
    // siblings for every attachment view of the source framebuffer. This
    // test pins that behaviour: registering only the base FB, a base RT0
    // view, and a single RMW pass with no further pinning steps still leaves
    // the RMW pass reachable through a name-based-latest reader.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    // Mirror what `RenderPipeline::PopulateBlackboard` does at frame start:
    // register the base SceneColor framebuffer and its canonical RT0 view.
    RGResourceDesc fbDesc;
    fbDesc.Kind = ResourceHandle::Kind::Framebuffer;
    fbDesc.Width = 1u;
    fbDesc.Height = 1u;
    fbDesc.Attachments = { RGResourceFormat::RGBA8UNorm };
    fbDesc.DebugName = std::string(ResourceNames::SceneColor);
    const auto sceneColorFB = graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, fbDesc);
    [[maybe_unused]] const auto baseSceneColorView = graph.CreateFramebufferAttachmentView(
        ResourceNames::SceneColorTexture, sceneColorFB, 0u);

    // Base SceneColor writer (stand-in for DeferredLightingPass).
    AddSetupNode(
        graph,
        "DeferredLightingPass",
        [sceneColorFB](RGBuilder& builder)
        {
            builder.Write(sceneColorFB, RGWriteUsage::RenderTarget);
        });

    // The pass under test. RMW on SceneColor with no other RMW step pinning
    // it; orphaned before the auto-publish fix.
    AddSetupNode(
        graph,
        "ForwardOverlayPass",
        [sceneColorFB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(sceneColorFB, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto newVer = builder.WriteNewVersion(
                sceneColorFB, RGWriteUsage::RenderTarget, "ForwardOverlayPass");
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        });

    // Post-process-style consumer: reads whatever the latest SceneColorTexture
    // version is, via the same name-based lookup that
    // `ReadFirstValidVersionedInputForPass` uses in production. Falls back to
    // the base view if no versioned sibling exists — which is exactly the
    // path that orphaned ForwardOverlay before this fix.
    AddSetupNode(
        graph,
        "DownstreamReaderPass",
        [](RGBuilder& builder)
        {
            const auto latestTex = builder.GetGraph().GetTextureHandle(ResourceNames::SceneColorTexture);
            if (latestTex.IsValid())
                [[maybe_unused]]
                const auto r = builder.Read(latestTex, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("DownstreamReaderPass");
    graph.BuildFrameGraph();

    // The auto-publish must have created the versioned attachment view so
    // the latest-version map for `SceneColorTexture` points at it.
    const auto versionedView = graph.GetTextureHandle("SceneColorTexture@ForwardOverlayPass");
    EXPECT_TRUE(versionedView.IsValid())
        << "WriteNewVersion on a framebuffer must auto-publish versioned attachment views";

    // ForwardOverlayPass must NOT be culled.
    const auto& culled = graph.GetCulledPasses();
    EXPECT_TRUE(std::ranges::find(culled, "ForwardOverlayPass") == culled.end())
        << "ForwardOverlayPass must remain reachable when no optional RMW step pins the SceneColor chain";

    const auto& order = graph.GetExecutionOrder();
    const auto overlayPos = std::ranges::find(order, "ForwardOverlayPass");
    const auto downstreamPos = std::ranges::find(order, "DownstreamReaderPass");
    const auto lightingPos = std::ranges::find(order, "DeferredLightingPass");

    ASSERT_NE(overlayPos, order.end()) << "ForwardOverlayPass missing from execution order";
    ASSERT_NE(downstreamPos, order.end()) << "DownstreamReaderPass missing from execution order";
    ASSERT_NE(lightingPos, order.end()) << "DeferredLightingPass missing from execution order";

    EXPECT_LT(lightingPos - order.begin(), overlayPos - order.begin())
        << "Base SceneColor writer must precede the RMW pass";
    EXPECT_LT(overlayPos - order.begin(), downstreamPos - order.begin())
        << "RMW pass must precede the downstream name-based reader";
}

TEST(RenderGraphSceneColorChain, EarlierRmwPassRemainsReachableWhenLaterRmwPassOverwritesLatestVersion)
{
    // Regression: when two RMW passes both call WriteNewVersion on the same
    // base resource, the SECOND one's version becomes the latest. Downstream
    // readers depend on the latest (second) writer. Without proper
    // last-writer tracking on the BASE name, the second pass's
    // `DependsOnPreviousWriter("SceneColor")` resolves to the *original
    // base writer* (e.g. DeferredLightingPass) instead of the previous
    // VERSIONED writer (the first RMW pass) — and the first RMW pass is
    // orphaned with "no downstream reader".
    //
    // This pins the fix in `processGraphNode`: when a versioned write
    // `X@tag` is recorded, the base name's last-writer entry is updated
    // to the same pass too, so the chain stays intact.
    //
    // Production scenario this catches: DeferredLighting → ForwardOverlay
    // → ParticlePass → AOApply / etc. — every RMW pass is reachable.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    RGResourceDesc fbDesc;
    fbDesc.Kind = ResourceHandle::Kind::Framebuffer;
    fbDesc.Width = 1u;
    fbDesc.Height = 1u;
    fbDesc.Attachments = { RGResourceFormat::RGBA8UNorm };
    fbDesc.DebugName = std::string(ResourceNames::SceneColor);
    const auto sceneColorFB = graph.DeclareTransientFramebuffer(ResourceNames::SceneColor, fbDesc);
    [[maybe_unused]] const auto baseSceneColorView = graph.CreateFramebufferAttachmentView(
        ResourceNames::SceneColorTexture, sceneColorFB, 0u);

    AddSetupNode(
        graph,
        "DeferredLightingPass",
        [sceneColorFB](RGBuilder& builder)
        {
            builder.Write(sceneColorFB, RGWriteUsage::RenderTarget);
        });

    // FIRST RMW pass — the one that historically got orphaned.
    AddSetupNode(
        graph,
        "ForwardOverlayPass",
        [sceneColorFB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(sceneColorFB, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto newVer = builder.WriteNewVersion(
                sceneColorFB, RGWriteUsage::RenderTarget, "ForwardOverlayPass");
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        });

    // SECOND RMW pass — overwrites the latest SceneColor version, then
    // calls DependsOnPreviousWriter to chain back. Must resolve to the
    // FIRST RMW pass (ForwardOverlayPass), not the base writer.
    AddSetupNode(
        graph,
        "ParticlePass",
        [sceneColorFB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(sceneColorFB, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto newVer = builder.WriteNewVersion(
                sceneColorFB, RGWriteUsage::RenderTarget, "ParticlePass");
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        });

    AddSetupNode(
        graph,
        "DownstreamReaderPass",
        [](RGBuilder& builder)
        {
            const auto latestTex = builder.GetGraph().GetTextureHandle(ResourceNames::SceneColorTexture);
            if (latestTex.IsValid())
                [[maybe_unused]]
                const auto r = builder.Read(latestTex, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("DownstreamReaderPass");
    graph.BuildFrameGraph();

    // Both RMW passes must remain reachable.
    const auto& culled = graph.GetCulledPasses();
    EXPECT_TRUE(std::ranges::find(culled, "ForwardOverlayPass") == culled.end())
        << "ForwardOverlayPass must stay reachable even when ParticlePass overwrites the latest SceneColor version";
    EXPECT_TRUE(std::ranges::find(culled, "ParticlePass") == culled.end())
        << "ParticlePass must stay reachable (it writes the latest SceneColor version)";

    // Verify chain ordering: DeferredLighting → ForwardOverlay → Particle → Downstream.
    const auto& order = graph.GetExecutionOrder();
    auto posOf = [&order](const char* name)
    { return std::ranges::find(order, name) - order.begin(); };

    ASSERT_NE(std::ranges::find(order, "ForwardOverlayPass"), order.end());
    ASSERT_NE(std::ranges::find(order, "ParticlePass"), order.end());

    EXPECT_LT(posOf("DeferredLightingPass"), posOf("ForwardOverlayPass"));
    EXPECT_LT(posOf("ForwardOverlayPass"), posOf("ParticlePass"))
        << "DependsOnPreviousWriter must chain ParticlePass back to ForwardOverlayPass, not skip to DeferredLightingPass";
    EXPECT_LT(posOf("ParticlePass"), posOf("DownstreamReaderPass"));
}

TEST(RenderGraphSceneColorChain, RmwContributorsRemainReachableWhenConsumerReadsBaseResource)
{
    // Regression: OIT-chain shape. OITPreparePass clears the BASE OITAccum;
    // DecalPass / ParticlePass do RMW via WriteNewVersion; OITResolvePass
    // reads the BASE OITAccum at the end (via typed handle, NOT via the
    // name-based latest-version cascade). Without the local
    // lastWriterByResource fix that propagates versioned writes back to the
    // base name's writer list, the consumer's Read→Writer derivation only
    // finds the original base writer (OITPreparePass) — the RMW
    // contributors are orphaned and culled as "no downstream reader", and
    // their writes never run even though their data is read at execute time.
    //
    // This pins the local-map base-name propagation in processGraphNode
    // (the analog of the m_LastWriterPassNameByResource fix above).
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    // Single-attachment FB stands in for the OIT MRT — same shape, fewer
    // dependencies in the test.
    RGResourceDesc fbDesc;
    fbDesc.Kind = ResourceHandle::Kind::Framebuffer;
    fbDesc.Width = 1u;
    fbDesc.Height = 1u;
    fbDesc.Attachments = { RGResourceFormat::RGBA8UNorm };
    fbDesc.DebugName = "TestRMWBuffer";
    const auto rmwFB = graph.DeclareTransientFramebuffer("TestRMWBuffer", fbDesc);
    [[maybe_unused]] const auto rmwBaseView = graph.CreateFramebufferAttachmentView(
        "TestRMWBufferView", rmwFB, 0u);

    // Base writer (clear pass — stand-in for OITPreparePass).
    AddSetupNode(
        graph,
        "ClearPass",
        [rmwFB](RGBuilder& builder)
        {
            builder.Write(rmwFB, RGWriteUsage::RenderTarget);
        });

    // First RMW contributor (stand-in for DecalPass).
    AddSetupNode(
        graph,
        "ContributorA",
        [rmwFB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(rmwFB, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto newVer = builder.WriteNewVersion(
                rmwFB, RGWriteUsage::RenderTarget, "ContributorA");
            builder.DependsOnPreviousWriter("TestRMWBuffer");
        });

    // Second RMW contributor (stand-in for ParticlePass).
    AddSetupNode(
        graph,
        "ContributorB",
        [rmwFB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(rmwFB, RGReadUsage::RenderTargetRead);
            [[maybe_unused]] const auto newVer = builder.WriteNewVersion(
                rmwFB, RGWriteUsage::RenderTarget, "ContributorB");
            builder.DependsOnPreviousWriter("TestRMWBuffer");
        });

    // Consumer reads the BASE handle directly — like OITResolvePass reading
    // `blackboard.OIT.OITAccum`. No name-based candidate cascade, no
    // DependsOnPreviousWriter. The contributors must still anchor via the
    // local lastWriterByResource map propagating versioned writes to the
    // base name.
    AddSetupNode(
        graph,
        "ConsumerPass",
        [rmwFB](RGBuilder& builder)
        {
            [[maybe_unused]] const auto r = builder.Read(rmwFB, RGReadUsage::ShaderSample);
        });

    graph.SetFinalPass("ConsumerPass");
    graph.BuildFrameGraph();

    const auto& culled = graph.GetCulledPasses();
    EXPECT_TRUE(std::ranges::find(culled, "ContributorA") == culled.end())
        << "ContributorA must be reachable through base-name Read on consumer (not just the original clear writer)";
    EXPECT_TRUE(std::ranges::find(culled, "ContributorB") == culled.end())
        << "ContributorB must be reachable through base-name Read on consumer";

    const auto& order = graph.GetExecutionOrder();
    auto posOf = [&order](const char* name)
    { return std::ranges::find(order, name) - order.begin(); };

    ASSERT_NE(std::ranges::find(order, "ContributorA"), order.end());
    ASSERT_NE(std::ranges::find(order, "ContributorB"), order.end());

    EXPECT_LT(posOf("ClearPass"), posOf("ContributorA"));
    EXPECT_LT(posOf("ContributorA"), posOf("ContributorB"));
    EXPECT_LT(posOf("ContributorB"), posOf("ConsumerPass"));
}

TEST(RenderGraphBuildDiagnostics, RegistrationOrderSensitivityIsReportedForReverseRmwChain)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "LatePass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                7u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "EarlyPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                7u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("EarlyPass");
    graph.BuildFrameGraph();

    const auto& stats = graph.GetLastBuildStats();
    EXPECT_EQ(stats.OrderSensitiveResults, 1u)
        << "Reversing the registration order of an otherwise identical SceneColor RMW pair should be reported exactly once.";

    const auto& diagnostics = graph.GetBuildDiagnostics();
    ASSERT_EQ(diagnostics.size(), 1u);

    const auto& diagnostic = diagnostics.front();
    EXPECT_EQ(diagnostic.Kind, RenderGraph::BuildDiagnosticKind::RegistrationOrderSensitivity);
    EXPECT_EQ(diagnostic.Resource, "SceneColor");
    EXPECT_EQ(diagnostic.CurrentBeforePass, "LatePass");
    EXPECT_EQ(diagnostic.CurrentAfterPass, "EarlyPass");
    EXPECT_EQ(diagnostic.AlternateBeforePass, "EarlyPass");
    EXPECT_EQ(diagnostic.AlternateAfterPass, "LatePass");
    EXPECT_NE(diagnostic.Message.find("SceneColor"), std::string::npos);
}

TEST(RenderGraphBuildDiagnostics, ExplicitDependencyRemovesRegistrationOrderSensitivity)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "LatePass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                8u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "EarlyPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                8u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    graph.AddExecutionDependency("EarlyPass", "LatePass");
    graph.SetFinalPass("LatePass");
    graph.BuildFrameGraph();

    const auto& stats = graph.GetLastBuildStats();
    EXPECT_EQ(stats.OrderSensitiveResults, 0u);
    EXPECT_TRUE(graph.GetBuildDiagnostics().empty())
        << "An explicit semantic ordering edge must eliminate registration-order-sensitive derived edges.";

    const auto& order = graph.GetExecutionOrder();
    const auto earlyPos = std::ranges::find(order, "EarlyPass") - order.begin();
    const auto latePos = std::ranges::find(order, "LatePass") - order.begin();
    EXPECT_LT(earlyPos, latePos);
}

TEST(RenderGraphBuildDiagnostics, SetupDeclaredPassDependencyRemovesRegistrationOrderSensitivity)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "LatePass",
        [](RGBuilder& builder)
        {
            builder.DependsOnPass("EarlyPass");

            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                9u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "EarlyPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                9u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("LatePass");
    graph.BuildFrameGraph();

    const auto& stats = graph.GetLastBuildStats();
    EXPECT_EQ(stats.OrderSensitiveResults, 0u);
    EXPECT_TRUE(graph.GetBuildDiagnostics().empty())
        << "A setup-declared semantic ordering edge must eliminate registration-order-sensitive derived edges.";

    const auto& order = graph.GetExecutionOrder();
    const auto earlyPos = std::ranges::find(order, "EarlyPass") - order.begin();
    const auto latePos = std::ranges::find(order, "LatePass") - order.begin();
    EXPECT_LT(earlyPos, latePos);
}

TEST(RenderGraphBuildDiagnostics, TransitiveSetupDependenciesSuppressRedundantRegistrationOrderSensitivity)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "LastPass",
        [](RGBuilder& builder)
        {
            builder.DependsOnPass("MiddlePass");

            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                10u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "MiddlePass",
        [](RGBuilder& builder)
        {
            builder.DependsOnPass("FirstPass");

            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                10u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "FirstPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                10u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("LastPass");
    graph.BuildFrameGraph();

    const auto& stats = graph.GetLastBuildStats();
    EXPECT_EQ(stats.OrderSensitiveResults, 0u);
    EXPECT_TRUE(graph.GetBuildDiagnostics().empty())
        << "Redundant direct edges that do not change the final semantic ordering must not count as registration-order sensitivity.";

    const auto& order = graph.GetExecutionOrder();
    const auto firstPos = std::ranges::find(order, "FirstPass") - order.begin();
    const auto middlePos = std::ranges::find(order, "MiddlePass") - order.begin();
    const auto lastPos = std::ranges::find(order, "LastPass") - order.begin();
    EXPECT_LT(firstPos, middlePos);
    EXPECT_LT(middlePos, lastPos);
}

// Mirrors the production SceneColor RMW chain (deferred path): seven passes —
// `SceneOrDeferredLighting` (initial producer) plus six modifiers that all
// read-modify-write `SceneColor`. Each modifier emits an explicit
// `DependsOnPass(previous_writer)`. The test registers the seven nodes in
// reverse order to ensure the explicit ordering edges, not registration order,
// are what pin topology — and asserts the build emits no
// RegistrationOrderSensitivity diagnostics.
TEST(RenderGraphBuildDiagnostics, SceneColorRMWChainIsRegistrationOrderIndependentWithExplicitDependsOnPass)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    constexpr std::array<const char*, 7u> chainNames = {
        "SceneOrDeferredLightingPass",
        "ForwardOverlayPass",
        "FoliagePass",
        "DecalPass",
        "WaterPass",
        "ParticlePass",
        "OITResolvePass",
    };

    auto makeNode = [&graph](const char* name, const char* previous)
    {
        AddSetupNode(
            graph,
            std::string(name),
            [previous](RGBuilder& builder)
            {
                auto sceneColor = builder.ImportTexture(
                    "SceneColor",
                    11u,
                    RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
                builder.AllowSamePassReadWrite(sceneColor);
                [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
                builder.Write(sceneColor, RGWriteUsage::RenderTarget);
                if (previous != nullptr)
                    builder.DependsOnPass(previous);
            });
    };

    // Register in REVERSE chain order to prove registration order is non-load-bearing.
    for (size_t i = chainNames.size(); i-- > 0;)
    {
        const char* previous = (i == 0u) ? nullptr : chainNames[i - 1u];
        makeNode(chainNames[i], previous);
    }

    graph.SetFinalPass("OITResolvePass");
    graph.BuildFrameGraph();

    const auto& stats = graph.GetLastBuildStats();
    EXPECT_EQ(stats.OrderSensitiveResults, 0u)
        << "Explicit DependsOnPass edges between consecutive SceneColor modifiers must eliminate registration-order sensitivity.";
    EXPECT_TRUE(graph.GetBuildDiagnostics().empty())
        << "Pinned modifier chain must not emit RegistrationOrderSensitivity diagnostics.";

    const auto& order = graph.GetExecutionOrder();
    std::vector<size_t> chainPositions;
    chainPositions.reserve(chainNames.size());
    for (const auto* name : chainNames)
    {
        const auto it = std::ranges::find(order, name);
        ASSERT_NE(it, order.end()) << "Chain pass missing from execution order: " << name;
        chainPositions.push_back(static_cast<size_t>(it - order.begin()));
    }
    for (size_t i = 1u; i < chainPositions.size(); ++i)
    {
        EXPECT_LT(chainPositions[i - 1u], chainPositions[i])
            << "Chain pass " << chainNames[i - 1u] << " must execute before " << chainNames[i];
    }
}

// Mirrors the production OIT contributor chain: `OITPrepare` clears
// `OITAccum`/`OITRevealage`, then `Decal` and `Particle` (when both are
// OIT-active) accumulate into them via RMW. WB-OIT is mathematically
// commutative across contributors, but the graph compiler still flags
// registration-order sensitivity for the Decal vs Particle pair when no
// explicit edge pins their order. Particle's Setup now emits an explicit
// `DependsOnPass(previousWriter)` derived via
// `builder.DependsOnPreviousWriter("OITAccum")`, which removes the diagnostic
// without changing math correctness.
TEST(RenderGraphBuildDiagnostics, OITAccumContributorChainIsRegistrationOrderIndependentWithExplicitDependsOnPass)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    // Register in REVERSE chain order to prove registration order is non-load-bearing.
    AddSetupNode(
        graph,
        "ParticlePass",
        [](RGBuilder& builder)
        {
            auto oitAccum = builder.ImportTexture(
                "OITAccum",
                12u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "OITAccum"));
            auto oitRevealage = builder.ImportTexture(
                "OITRevealage",
                13u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "OITRevealage"));
            builder.AllowSamePassReadWrite(oitAccum);
            builder.AllowSamePassReadWrite(oitRevealage);
            [[maybe_unused]] const auto accumRead = builder.Read(oitAccum, RGReadUsage::RenderTargetRead);
            builder.Write(oitAccum, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto revealageRead = builder.Read(oitRevealage, RGReadUsage::RenderTargetRead);
            builder.Write(oitRevealage, RGWriteUsage::RenderTarget);
            builder.DependsOnPass("OITPreparePass");
            builder.DependsOnPass("DecalPass");
        });

    AddSetupNode(
        graph,
        "DecalPass",
        [](RGBuilder& builder)
        {
            auto oitAccum = builder.ImportTexture(
                "OITAccum",
                12u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "OITAccum"));
            auto oitRevealage = builder.ImportTexture(
                "OITRevealage",
                13u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "OITRevealage"));
            builder.AllowSamePassReadWrite(oitAccum);
            builder.AllowSamePassReadWrite(oitRevealage);
            [[maybe_unused]] const auto accumRead = builder.Read(oitAccum, RGReadUsage::RenderTargetRead);
            builder.Write(oitAccum, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto revealageRead = builder.Read(oitRevealage, RGReadUsage::RenderTargetRead);
            builder.Write(oitRevealage, RGWriteUsage::RenderTarget);
            builder.DependsOnPass("OITPreparePass");
        });

    AddSetupNode(
        graph,
        "OITPreparePass",
        [](RGBuilder& builder)
        {
            auto oitAccum = builder.ImportTexture(
                "OITAccum",
                12u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "OITAccum"));
            auto oitRevealage = builder.ImportTexture(
                "OITRevealage",
                13u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "OITRevealage"));
            builder.Write(oitAccum, RGWriteUsage::Clear);
            builder.Write(oitRevealage, RGWriteUsage::Clear);
        });

    graph.SetFinalPass("ParticlePass");
    graph.BuildFrameGraph();

    const auto& stats = graph.GetLastBuildStats();
    EXPECT_EQ(stats.OrderSensitiveResults, 0u)
        << "Explicit DependsOnPass between OIT contributors must eliminate registration-order sensitivity for OITAccum/OITRevealage.";
    EXPECT_TRUE(graph.GetBuildDiagnostics().empty())
        << "Pinned OIT contributor chain must not emit RegistrationOrderSensitivity diagnostics.";

    const auto& order = graph.GetExecutionOrder();
    const auto preparePos = std::ranges::find(order, "OITPreparePass") - order.begin();
    const auto decalPos = std::ranges::find(order, "DecalPass") - order.begin();
    const auto particlePos = std::ranges::find(order, "ParticlePass") - order.begin();
    EXPECT_LT(preparePos, decalPos) << "OITPreparePass must execute before any contributor (derived from Clear → RenderTargetRead).";
    EXPECT_LT(preparePos, particlePos);
    EXPECT_LT(decalPos, particlePos) << "ParticlePass must execute after DecalPass (pinned by explicit DependsOnPass).";
}

TEST(RenderGraphBuildDiagnostics, DumpToJsonIncludesRegistrationOrderSensitivityDiagnostics)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddSetupNode(
        graph,
        "LatePass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                9u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "EarlyPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                9u,
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        });

    graph.SetFinalPass("EarlyPass");
    graph.BuildFrameGraph();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_build_diagnostics_dump.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());

    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find("\"buildDiagnosticCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"orderSensitiveResults\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"buildDiagnostics\": ["), std::string::npos);
    EXPECT_NE(json.find("\"kind\": \"RegistrationOrderSensitivity\""), std::string::npos);
    EXPECT_NE(json.find("\"resource\": \"SceneColor\""), std::string::npos);
    EXPECT_NE(json.find("\"currentBeforePass\": \"LatePass\""), std::string::npos);
    EXPECT_NE(json.find("\"currentAfterPass\": \"EarlyPass\""), std::string::npos);
    EXPECT_NE(json.find("\"alternateBeforePass\": \"EarlyPass\""), std::string::npos);
    EXPECT_NE(json.find("\"alternateAfterPass\": \"LatePass\""), std::string::npos);
}
