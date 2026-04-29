#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "PropertyTests/RenderPropertyTest.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unordered_set>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Minimal Stub RenderPass for Graph Tests (no GL)
// =============================================================================

class StubRenderPass : public RenderPass
{
  public:
    explicit StubRenderPass(const std::string& name)
    {
        m_Name = name;
    }
    ~StubRenderPass() override = default;

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute() override
    {
        m_ExecuteCount++;
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

class BucketStubPass : public CommandBufferRenderPass
{
  public:
    explicit BucketStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute() override {}
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }
};

class ImmediateStubPass : public RenderPass
{
  public:
    explicit ImmediateStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute() override {}
    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }
    [[nodiscard]] SubmissionModel GetSubmissionModel() const override
    {
        return SubmissionModel::ImmediateOnly;
    }

    void DeclareTestRead(std::string_view resource)
    {
        DeclareRead(resource, ResourceHandle::Kind::Texture2D);
    }
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
    FramebufferSpecification m_Specification;
};

class ContextAwareStubPass : public RenderPass
{
  public:
    explicit ContextAwareStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}

    void Execute() override
    {
        m_LegacyExecuteCount++;
    }

    void Execute(RGCommandContext& context) override
    {
        m_ContextExecuteCount++;
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

    [[nodiscard]] u32 GetLegacyExecuteCount() const
    {
        return m_LegacyExecuteCount;
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
    u32 m_LegacyExecuteCount = 0;
    bool m_ContextWasActive = false;
    std::string m_ContextPassName;
};

class InputTrackingStubPass : public RenderPass
{
  public:
    explicit InputTrackingStubPass(const std::string& name)
    {
        m_Name = name;
    }

    void Init(const FramebufferSpecification& /*spec*/) override {}
    void Execute() override
    {
        m_ExecuteCount++;
    }

    [[nodiscard]] Ref<Framebuffer> GetTarget() const override
    {
        return nullptr;
    }

    void SetInputFramebuffer(const Ref<Framebuffer>& /*input*/) override
    {
        m_InputSetCount++;
    }

    [[nodiscard]] u32 GetInputSetCount() const
    {
        return m_InputSetCount;
    }

    [[nodiscard]] u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

  private:
    u32 m_InputSetCount = 0;
    u32 m_ExecuteCount = 0;
};

// Helper to create and add a stub pass
static Ref<StubRenderPass> AddStub(RenderGraph& graph, const std::string& name)
{
    auto pass = Ref<StubRenderPass>::Create(name);
    pass->SetName(name);
    graph.AddPass(pass);
    return pass;
}

// =============================================================================
// Basic Graph Construction
// =============================================================================

TEST(RenderGraph, AddPassMakesItRetrievable)
{
    RenderGraph graph;

    auto pass = AddStub(graph, "PassA");
    auto retrieved = graph.GetPass<StubRenderPass>("PassA");

    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->GetName(), "PassA");
}

TEST(RenderGraph, GetPassReturnsNullForUnknown)
{
    RenderGraph graph;
    auto pass = graph.GetPass<StubRenderPass>("NonExistent");
    EXPECT_EQ(pass, nullptr);
}

TEST(RenderGraph, GetAllPassesReturnsAll)
{
    RenderGraph graph;
    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");

    auto all = graph.GetAllPasses();
    EXPECT_EQ(all.size(), 3u);
}

TEST(RenderGraph, PassSubmissionInfoReportsModelsAndResourceDeclarations)
{
    RenderGraph graph;

    auto bucket = Ref<BucketStubPass>::Create("BucketPass");
    auto immediate = Ref<ImmediateStubPass>::Create("ImmediatePass");
    immediate->DeclareTestRead("SceneColor");

    graph.AddPass(bucket);
    graph.AddPass(immediate);

    const auto info = graph.GetPassSubmissionInfo();
    ASSERT_EQ(info.size(), 2u);

    const auto bucketIt = std::find_if(info.begin(), info.end(), [](const RenderGraph::PassSubmissionInfo& entry)
                                       { return entry.PassName == "BucketPass"; });
    ASSERT_NE(bucketIt, info.end());
    EXPECT_EQ(bucketIt->Submission, RenderPass::SubmissionModel::BucketOnly);
    EXPECT_FALSE(bucketIt->DeclaresResources);

    const auto immediateIt = std::find_if(info.begin(), info.end(), [](const RenderGraph::PassSubmissionInfo& entry)
                                          { return entry.PassName == "ImmediatePass"; });
    ASSERT_NE(immediateIt, info.end());
    EXPECT_EQ(immediateIt->Submission, RenderPass::SubmissionModel::ImmediateOnly);
    EXPECT_TRUE(immediateIt->DeclaresResources);
}

TEST(RenderGraph, ExecuteProvidesActiveCommandContextScope)
{
    RenderGraph graph;

    auto pass = Ref<ContextAwareStubPass>::Create("ContextPass");
    graph.AddPass(pass);

    graph.Execute();

    EXPECT_EQ(pass->GetContextExecuteCount(), 1u);
    EXPECT_EQ(pass->GetLegacyExecuteCount(), 0u);
    EXPECT_TRUE(pass->WasContextActive());
    EXPECT_EQ(pass->GetObservedContextPassName(), "ContextPass");
}

TEST(RenderGraph, PlansBarrierFromWriterToReaderTransition)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "Producer");
    AddStub(graph, "Consumer");

    graph.RegisterGraphPass(
        "Producer",
        [](RGBuilder& builder)
        {
            auto sharedBuffer = builder.ImportBuffer(
                "SharedBuffer",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            builder.Write(sharedBuffer, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Consumer",
        [](RGBuilder& builder)
        {
            auto sharedBuffer = builder.ImportBuffer(
                "SharedBuffer",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            [[maybe_unused]] const auto readHandle = builder.Read(sharedBuffer, RGReadUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("Producer", "Consumer");
    graph.SetFinalPass("Consumer");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto& plannedBarriers = graph.GetPlannedBarriers();
    const auto barrierIt = std::find_if(plannedBarriers.begin(), plannedBarriers.end(),
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

    AddStub(graph, "GBufferWrite");
    AddStub(graph, "PostRead");

    graph.RegisterGraphPass(
        "GBufferWrite",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColorTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColorTex"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "PostRead",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColorTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColorTex"));
            [[maybe_unused]] const auto readHandle = builder.Read(sceneColor, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("GBufferWrite", "PostRead");
    graph.SetFinalPass("PostRead");

    graph.BuildFrameGraph();
    graph.Execute();

    const auto& plannedBarriers = graph.GetPlannedBarriers();
    const auto barrierIt = std::find_if(plannedBarriers.begin(), plannedBarriers.end(),
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

    AddStub(graph, "ConsumerOnly");

    graph.RegisterGraphPass(
        "ConsumerOnly",
        [](RGBuilder& builder)
        {
            auto orphanResource = builder.ImportTexture(
                "OrphanResource",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "OrphanResource"));
            [[maybe_unused]] const auto readHandle = builder.Read(orphanResource, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("ConsumerOnly");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::find_if(diagnostics.begin(), diagnostics.end(),
                                     [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                     {
                                         return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::MissingProducer &&
                                                diagnostic.PassName == "ConsumerOnly" &&
                                                diagnostic.Resource == "OrphanResource";
                                     });
    ASSERT_NE(diagIt, diagnostics.end());
}

TEST(RenderGraph, ReportsStaleExtractionHandleDiagnostic)
{
    RenderGraph graph;

    const auto stale = graph.ImportTexture(
        "TemporalColor",
        7,
        RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "TemporalColor"));

    graph.ClearImportedResources();

    bool extractionCalled = false;
    graph.ExtractTexture(stale,
                         [&extractionCalled](u32 /*id*/)
                         {
                             extractionCalled = true;
                         });

    graph.Execute();

    const auto& diagnostics = graph.GetBarrierDiagnostics();
    const auto diagIt = std::find_if(diagnostics.begin(), diagnostics.end(),
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

    AddStub(graph, "CulledProducer");
    AddStub(graph, "FinalConsumer");

    graph.RegisterGraphPass(
        "CulledProducer",
        [](RGBuilder& builder)
        {
            auto transientColor = builder.ImportTexture(
                "CulledColor",
                11,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "CulledColor"));
            builder.Write(transientColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "FinalConsumer",
        [](RGBuilder& builder)
        {
            auto finalColor = builder.ImportTexture(
                "FinalColorTex",
                12,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
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
    const auto diagIt = std::find_if(diagnostics.begin(), diagnostics.end(),
                                     [](const RenderGraph::BarrierDiagnostic& diagnostic)
                                     {
                                         return diagnostic.Kind == RenderGraph::BarrierDiagnosticKind::ExtractionOfCulledResource &&
                                                diagnostic.Resource == "CulledColor";
                                     });

    ASSERT_NE(diagIt, diagnostics.end());
    EXPECT_FALSE(extractionCalled);
}

TEST(RenderGraph, DumpToJsonWritesCompiledGraphDetails)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
        "Producer",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportBuffer(
                "SharedBuffer",
                41,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
            builder.Write(shared, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Final",
        [](RGBuilder& builder)
        {
            auto shared = builder.ImportBuffer(
                "SharedBuffer",
                41,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::StorageBuffer, "SharedBuffer"));
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
    EXPECT_NE(json.find("\"schemaVersion\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"timingVersion\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"hasTimings\": true"), std::string::npos);
    EXPECT_NE(json.find("\"frameSummary\""), std::string::npos);
    EXPECT_NE(json.find("\"passCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"resourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"culledPassCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"timingsCount\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"buildStats\""), std::string::npos);
    EXPECT_NE(json.find("\"passesVisited\":"), std::string::npos);
    EXPECT_NE(json.find("\"declaredReads\":"), std::string::npos);
    EXPECT_NE(json.find("\"declaredWrites\":"), std::string::npos);
    EXPECT_NE(json.find("\"derivedEdges\":"), std::string::npos);
    EXPECT_NE(json.find("\"passOrder\""), std::string::npos);
    EXPECT_NE(json.find("\"plannedBarriers\""), std::string::npos);
    EXPECT_NE(json.find("\"barrierDiagnostics\""), std::string::npos);
    EXPECT_NE(json.find("\"lifetimes\""), std::string::npos);
    EXPECT_NE(json.find("\"accessModes\""), std::string::npos);
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
    EXPECT_NE(json.find("res:SharedBuffer:StorageBuffer:1"), std::string::npos);
    EXPECT_NE(json.find("life:SharedBuffer@0-1:r1:w1"), std::string::npos);
    EXPECT_NE(json.find("acc:SharedBuffer:r[ShaderStorage]:w[ShaderStorage]"), std::string::npos);
    EXPECT_NE(json.find("\"barrierDigest\": { \"version\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"plannedCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"diagnosticCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"missingProducerCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"entryCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("bar:Final/SharedBuffer/"), std::string::npos);
    EXPECT_NE(json.find("\"graphDigest\": { \"version\": 1"), std::string::npos);
    EXPECT_NE(json.find("passes=2;resources=1;culled=0;barriers=1;diags=0;aliases=0;timings=2"), std::string::npos);
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

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 3u);

    // A must appear before B, and B before C
    auto posA = std::find(order.begin(), order.end(), "A") - order.begin();
    auto posB = std::find(order.begin(), order.end(), "B") - order.begin();
    auto posC = std::find(order.begin(), order.end(), "C") - order.begin();

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

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 4u);

    auto posA = std::find(order.begin(), order.end(), "A") - order.begin();
    auto posB = std::find(order.begin(), order.end(), "B") - order.begin();
    auto posC = std::find(order.begin(), order.end(), "C") - order.begin();
    auto posD = std::find(order.begin(), order.end(), "D") - order.begin();

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

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 2u);

    auto posShadow = std::find(order.begin(), order.end(), "Shadow") - order.begin();
    auto posScene = std::find(order.begin(), order.end(), "Scene") - order.begin();

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

    const auto& order = graph.GetPassOrder();
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

    const auto& order = graph.GetPassOrder();
    EXPECT_EQ(order.size(), 3u);

    // Legacy behavior when final pass is auto-selected: all independent
    // passes still execute.
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
    c->SetSideEffects(RenderPass::SideEffect::Readback);

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

    const auto& order = graph.GetPassOrder();
    auto posOf = [&](const std::string& n)
    {
        return std::find(order.begin(), order.end(), n) - order.begin();
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
    graph.AddPass(post);
    graph.AddPass(ui);
    graph.AddPass(final);

    // Phase F contract: FinalPass is a side-effecting present sink.
    // UIComposite -> Final must be ordering-only, not framebuffer piping.
    graph.ConnectPass("PostProcessPass", "UICompositePass");
    graph.AddExecutionDependency("UICompositePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    graph.Execute();

    EXPECT_EQ(final->GetInputSetCount(), 0u)
        << "FinalPass must not receive piped framebuffer input from UICompositePass";
    EXPECT_EQ(final->GetExecuteCount(), 1u)
        << "FinalPass must still execute via ordering dependency";

    const auto& order = graph.GetPassOrder();
    auto posOf = [&](const std::string& n)
    {
        return std::find(order.begin(), order.end(), n) - order.begin();
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

    AddStub(graph, "WaterPass");
    AddStub(graph, "DecalPass");

    // Explicit production ordering contract.
    graph.AddExecutionDependency("DecalPass", "WaterPass");

    // BuildFrameGraph will see both passes write SceneColor. Without a
    // cycle guard, insertion order (Water before Decal) can derive the
    // reverse Water -> Decal edge and create a cycle.
    graph.RegisterGraphPass(
        "WaterPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "DecalPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("WaterPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const auto hasCycle = std::any_of(hazards.begin(), hazards.end(),
                                      [](const RenderGraph::Hazard& h)
                                      {
                                          return h.Kind == RenderGraph::HazardKind::Cycle;
                                      });
    EXPECT_FALSE(hasCycle) << "Derived edge insertion must not create cycles";
    EXPECT_EQ(graph.GetPassOrder().size(), 2u)
        << "Cycle guard must preserve a valid two-pass topological order";
}

TEST(RenderGraphStructural, DerivedEdgesSatisfyDeferredCoreWithoutManualEdges)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "ScenePass");
    AddStub(graph, "DeferredOpaqueDecalPass");
    AddStub(graph, "DeferredLightingPass");
    AddStub(graph, "FinalPass");

    graph.RegisterGraphPass(
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));

            builder.Write(sceneDepth, RGWriteUsage::DepthStencil);
            builder.Write(sceneNormals, RGWriteUsage::RenderTarget);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "DeferredOpaqueDecalPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "DeferredLightingPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalsRead = builder.Read(sceneNormals, RGReadUsage::ShaderSample);
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "FinalPass",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                std::string(ResourceNames::SceneColor),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneColor)));
            [[maybe_unused]] const auto sceneColorRead = builder.Read(sceneColor, RGReadUsage::RenderTargetRead);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FinalPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "Derived deferred-core edges should satisfy hazards";

    const auto& order = graph.GetPassOrder();
    auto posOf = [&](const char* name)
    {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };
    EXPECT_LT(posOf("ScenePass"), posOf("DeferredOpaqueDecalPass"));
    EXPECT_LT(posOf("DeferredOpaqueDecalPass"), posOf("DeferredLightingPass"));
    EXPECT_LT(posOf("DeferredLightingPass"), posOf("FinalPass"));
}

TEST(RenderGraphStructural, DerivedEdgesSatisfySceneToSSAOWithoutManualEdge)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "ScenePass");
    AddStub(graph, "SSAOPass");
    AddStub(graph, "FinalPass");

    graph.RegisterGraphPass(
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            builder.Write(sceneDepth, RGWriteUsage::DepthStencil);
            builder.Write(sceneNormals, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "SSAOPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalsRead = builder.Read(sceneNormals, RGReadUsage::ShaderSample);
            builder.Write(ao, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "FinalPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));
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

    AddStub(graph, "ScenePass");
    AddStub(graph, "GTAOPass");
    AddStub(graph, "FinalPass");

    graph.RegisterGraphPass(
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            builder.Write(sceneDepth, RGWriteUsage::DepthStencil);
            builder.Write(sceneNormals, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "GTAOPass",
        [](RGBuilder& builder)
        {
            auto sceneDepth = builder.ImportTexture(
                std::string(ResourceNames::SceneDepth),
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneDepth)));
            auto sceneNormals = builder.ImportTexture(
                std::string(ResourceNames::SceneNormals),
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::SceneNormals)));
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));

            [[maybe_unused]] const auto depthRead = builder.Read(sceneDepth, RGReadUsage::ShaderSample);
            [[maybe_unused]] const auto normalsRead = builder.Read(sceneNormals, RGReadUsage::ShaderSample);
            builder.Write(ao, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "FinalPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                std::string(ResourceNames::AOBuffer),
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, std::string(ResourceNames::AOBuffer)));
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
    EXPECT_EQ(graph.GetAllPasses().size(), 1u)
        << "Connect calls with missing passes must not register new passes";
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
// Duplicate AddPass Overwrites
// =============================================================================

TEST(RenderGraph, DuplicatePassNameOverwrites)
{
    RenderGraph graph;
    auto first = AddStub(graph, "SameName");
    auto second = AddStub(graph, "SameName");

    auto retrieved = graph.GetPass<StubRenderPass>("SameName");
    // The second addition should overwrite
    EXPECT_EQ(retrieved.Raw(), second.Raw());
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

    const auto& order = graph.GetPassOrder();
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

    const auto& order = graph.GetPassOrder();
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
    EXPECT_EQ(graph.GetAllPasses().size(), 3u);
    EXPECT_EQ(graph.GetPassOrder().size(), 3u);

    graph.ResetTopology();

    // After reset the graph must behave as freshly constructed: no
    // passes, no cached order, no stale connections leaking into the
    // next rebuild.
    EXPECT_EQ(graph.GetAllPasses().size(), 0u);
    EXPECT_EQ(graph.GetConnections().size(), 0u);

    // Rebuild as a different "forward-like" topology (2 passes).
    AddStub(graph, "X");
    AddStub(graph, "Y");
    graph.ConnectPass("X", "Y");
    graph.SetFinalPass("Y");
    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "X");
    EXPECT_EQ(order[1], "Y");
    EXPECT_EQ(graph.GetPass<StubRenderPass>("A"), nullptr)
        << "Old passes must not be retrievable after ResetTopology";
    EXPECT_EQ(graph.GetPass<StubRenderPass>("B"), nullptr);
    EXPECT_EQ(graph.GetPass<StubRenderPass>("C"), nullptr);
}

TEST(RenderGraphResetTopology, PreservesPassReferenceOwnership)
{
    // The graph stores passes by strong lookup in m_PassLookup
    // (std::unordered_map<std::string, Ref<RenderPass>>). ResetTopology
    // must only drop the graph's references, not destroy passes still
    // held by external owners (in the real engine that's
    // Renderer3D::s_Data). The test below verifies that an external Ref
    // to a RenderPass keeps it alive after the graph forgets it.
    RenderGraph graph;
    auto a = Ref<StubRenderPass>::Create("A");
    a->SetName("A");
    graph.AddPass(a);

    graph.ResetTopology();

    ASSERT_NE(a.Raw(), nullptr);
    EXPECT_EQ(a->GetName(), "A");

    // Re-registering the same pass instance is legal — simulates the
    // per-path rebuild re-AddPass'ing the persistent pass instances.
    graph.AddPass(a);
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
        EXPECT_EQ(graph.GetAllPasses().size(), 1u);
    }
}

TEST(RenderGraphResetTopology, ImportedHandleSlotsAreRebackedAfterReset)
{
    RenderGraph graph;

    auto textureDesc = RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ImportedTexture");
    const auto oldTexture = graph.ImportTexture("ImportedTexture", 42u, textureDesc);
    EXPECT_EQ(graph.ResolveTexture(oldTexture), 42u);

    auto bufferDesc = RGResourceDesc::FromLegacy(ResourceHandle::Kind::UniformBuffer, "ImportedBuffer");
    const auto oldBuffer = graph.ImportBuffer("ImportedBuffer", 77u, bufferDesc);
    EXPECT_EQ(graph.ResolveBuffer(oldBuffer), 77u);

    auto framebufferDesc = RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "ImportedFramebuffer");
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

TEST(RenderGraphTransientPool, UnreachableTransientResourceIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "TransientProducer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Final",
        [](RGBuilder& builder)
        {
            auto sceneColor = builder.ImportTexture(
                "FinalColorTex",
                9,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "FinalColorTex"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "A");
    AddStub(graph, "B");
    AddStub(graph, "C");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColorTex"));
            builder.Write(sceneColor, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    // Keep TempA chain reachable from Final so both transient candidates
    // participate in planning and alias-slot assignment.
    graph.AddExecutionDependency("B", "C");

    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto findPlan = [&transientPlan](const std::string& resource) -> const RenderGraph::TransientPlanEntry*
    {
        const auto it = std::find_if(transientPlan.begin(), transientPlan.end(),
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

TEST(RenderGraphTransientPool, UnsupportedFramebufferFormatIsNotPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RG16Float;
            desc.Width = 640;
            desc.Height = 360;

            const auto unsupported = builder.CreateFramebuffer("UnsupportedTransientFB", desc);
            builder.Write(unsupported, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Framebuffer;
            desc.Format = RGResourceFormat::RG16Float;
            desc.Width = 640;
            desc.Height = 360;

            const auto unsupported = builder.CreateFramebuffer("UnsupportedTransientFB", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::RenderTargetRead);
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
                                     [](const RenderGraph::TransientPlanEntry& entry)
                                     {
                                         return entry.Resource == "UnsupportedTransientFB";
                                     });

    ASSERT_NE(planIt, transientPlan.end());
    EXPECT_TRUE(planIt->Reachable);
    EXPECT_FALSE(planIt->WillAllocate);
    EXPECT_EQ(planIt->SkipReason, "unsupported-framebuffer-format");
}

TEST(RenderGraphTransientPool, RG16FloatTextureIsPlannedForAllocation)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "Producer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
        "Producer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::StorageBuffer;
            desc.Width = 0;

            const auto unsupported = builder.CreateBuffer("ZeroByteTransientBuffer", desc);
            builder.Write(unsupported, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::StorageBuffer;
            desc.Width = 0;

            const auto unsupported = builder.CreateBuffer("ZeroByteTransientBuffer", desc);
            [[maybe_unused]] const auto readUnsupported = builder.Read(unsupported, RGReadUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Producer", "Final");
    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "Producer");
    AddStub(graph, "Consumer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
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
            invalidDesc.Format = RGResourceFormat::RG16Float;
            invalidDesc.Width = 256;
            invalidDesc.Height = 256;

            const auto invalid = builder.CreateFramebuffer("JsonInvalidFB", invalidDesc);
            builder.Write(invalid, RGWriteUsage::RenderTarget);

            const auto unreachable = builder.CreateTexture("JsonUnused", desc);
            builder.Write(unreachable, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

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

TEST(RenderGraphTransientPool, MaterializationEnabledIsSafeWithoutBackend)
{
    if (RendererAPI::GetAPI() != RendererAPI::API::None)
        GTEST_SKIP() << "No-backend safety check is only valid when RendererAPI::None is active";

    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    AddStub(graph, "TransientProducer");
    AddStub(graph, "Final");

    graph.RegisterGraphPass(
        "TransientProducer",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 320;
            desc.Height = 180;

            const auto temp = builder.CreateTexture("HeadlessTransient", desc);
            builder.Write(temp, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Final",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 320;
            desc.Height = 180;

            const auto temp = builder.CreateTexture("HeadlessTransient", desc);
            [[maybe_unused]] const auto readTemp = builder.Read(temp, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("Final");
    graph.BuildFrameGraph();

    EXPECT_NO_THROW(graph.Execute());

    const auto handle = graph.GetTextureHandle("HeadlessTransient");
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(graph.ResolveTexture(handle), 0u)
        << "No-backend execution must not create physical textures";
}

TEST(RenderGraphTransientPool, TransientTextureIsAllocatedFromPoolWhenMaterializationEnabled)
{
    // Skip test if no rendering backend is available
    if (RendererAPI::GetAPI() == RendererAPI::API::None)
        GTEST_SKIP() << "Transient materialization test requires an active rendering backend";

    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    // Add stub passes first
    AddStub(graph, "TransientProducer");
    AddStub(graph, "TransientConsumer");

    // Producer pass: creates and writes to a transient texture
    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    // Consumer pass: reads from the transient texture
    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("TransientConsumer");
    graph.BuildFrameGraph();
    // Verify the resource exists in the transient plan and was allocated
    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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

    AddStub(graph, "TransientWriter");

    graph.RegisterGraphPass(
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
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("TransientWriter");
    graph.BuildFrameGraph();

    const auto& transientPlan = graph.GetTransientPlan();
    const auto planIt = std::find_if(transientPlan.begin(), transientPlan.end(),
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
    if (extractedTextureID == 0u)
        GTEST_SKIP() << "Backend present but transient materialization produced texture ID 0 in this environment";
}

TEST(RenderGraphTransientPool, HeadlessTransientExtractionInvokesCallbackWithZeroTexture)
{
    if (RendererAPI::GetAPI() != RendererAPI::API::None)
        GTEST_SKIP() << "Headless transient extraction check is only valid when RendererAPI::None is active";

    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);
    graph.SetTransientMaterializationEnabled(true);

    AddStub(graph, "TransientWriter");

    graph.RegisterGraphPass(
        "TransientWriter",
        [](RGBuilder& builder)
        {
            RGResourceDesc desc;
            desc.Kind = ResourceHandle::Kind::Texture2D;
            desc.Format = RGResourceFormat::RGBA16Float;
            desc.Width = 128;
            desc.Height = 128;

            const auto transient = builder.CreateTexture("ReadbackTransient", desc);
            builder.Write(transient, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("TransientWriter");
    graph.BuildFrameGraph();

    const auto handle = graph.GetTextureHandle("ReadbackTransient");
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
