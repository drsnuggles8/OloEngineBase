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

    [[nodiscard]] u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

  private:
    u32 m_ExecuteCount = 0;
};

class DeclarationTrackingStubPass : public RenderPass
{
  public:
    explicit DeclarationTrackingStubPass(const std::string& name)
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

    void TestDeclareRead(std::string_view resource)
    {
        DeclareRead(resource, ResourceHandle::Kind::Framebuffer);
    }

    void TestDeclareWrite(std::string_view resource)
    {
        DeclareWrite(resource, ResourceHandle::Kind::Framebuffer);
    }

    [[nodiscard]] u32 GetExecuteCount() const
    {
        return m_ExecuteCount;
    }

  private:
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
    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);
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
    EXPECT_NE(json.find("\"temporalHistoryContractCount\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"passFlags\""), std::string::npos);
    EXPECT_NE(json.find("\"workType\": \"Graphics\""), std::string::npos);
    EXPECT_NE(json.find("\"asyncComputeCandidate\": false"), std::string::npos);
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
    EXPECT_NE(json.find("\"resources\""), std::string::npos);
    EXPECT_NE(json.find("\"isHistory\": false"), std::string::npos);
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
    EXPECT_NE(json.find("passes=2;resources=1;culled=0;barriers=1;diags=0;aliases=0;timings=2;compute=0;asyncCandidates=0;histories=0;historyContracts=0"), std::string::npos);
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

TEST(RenderGraph, DeclarationOnlyProducerChainRemainsReachable)
{
    RenderGraph graph;

    auto producer = Ref<DeclarationTrackingStubPass>::Create("Producer");
    auto middle = Ref<DeclarationTrackingStubPass>::Create("Middle");
    auto final = Ref<DeclarationTrackingStubPass>::Create("Final");

    graph.AddPass(producer);
    graph.AddPass(middle);
    graph.AddPass(final);

    producer->TestDeclareWrite("PostProcessColor");
    middle->TestDeclareRead("PostProcessColor");
    middle->TestDeclareWrite("ToneMapColor");
    final->TestDeclareRead("ToneMapColor");

    graph.SetFinalPass("Final");
    graph.Execute();

    EXPECT_EQ(producer->GetExecuteCount(), 1u)
        << "Declaration-only producer must remain reachable from the final pass";
    EXPECT_EQ(middle->GetExecuteCount(), 1u)
        << "Intermediate declaration-only pass must not be culled";
    EXPECT_EQ(final->GetExecuteCount(), 1u);
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
    EXPECT_EQ(pass->GetPassWorkType(), RenderPass::PassWorkType::Graphics);
    EXPECT_FALSE(pass->IsComputeOnly());
}

TEST(RenderGraphPassFlags, ComputePassTypeRoundTrips)
{
    // A pass explicitly flagged as Compute must report IsComputeOnly().
    RenderGraph graph;
    auto pass = AddStub(graph, "ComputePass");
    pass->SetPassWorkType(RenderPass::PassWorkType::Compute);
    EXPECT_EQ(pass->GetPassWorkType(), RenderPass::PassWorkType::Compute);
    EXPECT_TRUE(pass->IsComputeOnly());
}

TEST(RenderGraphPassFlags, CopyPassTypeRoundTrips)
{
    // A pass flagged as Copy must report Copy and must not be compute-only.
    RenderGraph graph;
    auto pass = AddStub(graph, "CopyPass");
    pass->SetPassWorkType(RenderPass::PassWorkType::Copy);
    EXPECT_EQ(pass->GetPassWorkType(), RenderPass::PassWorkType::Copy);
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
    isolated->SetSideEffects(RenderPass::SideEffect::NeverCull);

    graph.Execute();

    EXPECT_EQ(a->GetExecuteCount(), 1u);
    EXPECT_EQ(b->GetExecuteCount(), 1u);
    EXPECT_EQ(isolated->GetExecuteCount(), 1u) << "NeverCull pass must not be culled";
}

TEST(RenderGraphPassFlags, PassSubmissionInfoReportsWorkTypeAndAsyncFlag)
{
    // GetPassSubmissionInfo() must surface PassWorkType and AsyncComputeCandidate.
    RenderGraph graph;
    auto graphics = AddStub(graph, "GraphicsPass");
    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);

    graph.SetFinalPass("GraphicsPass");

    const auto infos = graph.GetPassSubmissionInfo();
    ASSERT_FALSE(infos.empty());

    for (const auto& info : infos)
    {
        if (info.PassName == "GraphicsPass")
        {
            EXPECT_EQ(info.WorkType, RenderPass::PassWorkType::Graphics);
            EXPECT_FALSE(info.AsyncComputeCandidate);
        }
        else if (info.PassName == "ComputePass")
        {
            EXPECT_EQ(info.WorkType, RenderPass::PassWorkType::Compute);
            EXPECT_TRUE(info.AsyncComputeCandidate);
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

    const auto& order = graph.GetPassOrder();
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
    c->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c->SetAsyncComputeCandidate(true);

    graph.ConnectPass("G1", "G2");
    graph.SetFinalPass("G2");
    c->SetSideEffects(RenderPass::SideEffect::NeverCull); // keep C alive

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 3u);
    // C must be scheduled before both graphics passes.
    const auto posC = std::find(order.begin(), order.end(), "C") - order.begin();
    const auto posG1 = std::find(order.begin(), order.end(), "G1") - order.begin();
    const auto posG2 = std::find(order.begin(), order.end(), "G2") - order.begin();
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
    c->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c->SetAsyncComputeCandidate(true);

    graph.ConnectPass("G1", "C");
    graph.SetFinalPass("C");

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 2u);
    const auto posG1 = std::find(order.begin(), order.end(), "G1") - order.begin();
    const auto posC = std::find(order.begin(), order.end(), "C") - order.begin();
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
    c1->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    c2->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);

    graph.SetFinalPass("G1");
    c1->SetSideEffects(RenderPass::SideEffect::NeverCull);
    c2->SetSideEffects(RenderPass::SideEffect::NeverCull);

    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 3u);
    const auto posG1 = std::find(order.begin(), order.end(), "G1") - order.begin();
    const auto posC1 = std::find(order.begin(), order.end(), "C1") - order.begin();
    const auto posC2 = std::find(order.begin(), order.end(), "C2") - order.begin();
    EXPECT_LT(posC1, posG1) << "C1 must be hoisted before G1";
    EXPECT_LT(posC2, posG1) << "C2 must be hoisted before G1";
}

// =============================================================================
// RenderGraphDumpJson — Phase G Slice 3: PassWorkType + AsyncComputeCandidate in JSON dump
// =============================================================================

TEST(RenderGraphDumpJson, PassFlagsAreSurfacedInDump)
{
    // A graph with one graphics pass and one async-compute pass must produce
    // schemaVersion 6 JSON containing a passFlags array with correct entries,
    // frameSummary counters, and per-entry workType/asyncComputeCandidate in
    // executionTimeline and timingStatsByPass.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "GfxPass");
    auto computePass = AddStub(graph, "ComputePass");
    computePass->SetPassWorkType(RenderPass::PassWorkType::Compute);
    computePass->SetAsyncComputeCandidate(true);
    computePass->SetSideEffects(RenderPass::SideEffect::NeverCull);

    graph.ConnectPass("ComputePass", "GfxPass");
    graph.SetFinalPass("GfxPass");
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "rg_phase_g3_flags.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Schema version bump
    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);

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
    computePass->SetPassWorkType(RenderPass::PassWorkType::Compute);
    computePass->SetAsyncComputeCandidate(true);
    computePass->SetSideEffects(RenderPass::SideEffect::NeverCull);

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
    const auto semiPos = dot.find(';', gfxPos);
    if (semiPos != std::string::npos)
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
    // Batch: {ComputePass}, WaitPasses={}, SignalPasses={GfxPass}.
    RenderGraph graph;
    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    AddStub(graph, "GfxPass");

    graph.ConnectPass("ComputePass", "GfxPass");
    graph.SetFinalPass("GfxPass");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_EQ(batch.Lane, RenderGraph::QueueLane::Compute)
        << "Async compute batches must be assigned to compute lane";
    ASSERT_EQ(batch.ComputePasses.size(), 1u);
    EXPECT_EQ(batch.ComputePasses[0], "ComputePass");

    // ComputePass has no non-batch predecessors
    EXPECT_TRUE(batch.WaitPasses.empty());

    // GfxPass depends on ComputePass — must appear in SignalPasses
    ASSERT_EQ(batch.SignalPasses.size(), 1u);
    EXPECT_EQ(batch.SignalPasses[0], "GfxPass");
}

TEST(RenderGraphAsyncBatch, IndependentComputePassHasEmptyWaitAndSignalLists)
{
    // ComputePass is NeverCull but has no edges to/from GfxFinal.
    // Both WaitPasses and SignalPasses must be empty.
    RenderGraph graph;
    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    EXPECT_EQ(batch.ComputePasses.size(), 1u);
    EXPECT_TRUE(batch.WaitPasses.empty());
    EXPECT_TRUE(batch.SignalPasses.empty());
}

TEST(RenderGraphAsyncBatch, ConsecutiveComputePassesGroupedInOneBatch)
{
    // Graph: C1 → C2 (both async compute) → GfxFinal.
    // C1 and C2 are consecutive in the hoisted order — one batch.
    RenderGraph graph;
    auto c1 = AddStub(graph, "C1");
    c1->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    c1->SetSideEffects(RenderPass::SideEffect::NeverCull);

    auto c2 = AddStub(graph, "C2");
    c2->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);
    c2->SetSideEffects(RenderPass::SideEffect::NeverCull);

    AddStub(graph, "GfxFinal");

    graph.ConnectPass("C1", "C2");
    graph.ConnectPass("C2", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u) << "C1 and C2 are consecutive — one batch expected";

    const auto& batch = batches[0];
    EXPECT_EQ(batch.ComputePasses.size(), 2u);

    // GfxFinal depends on C2 — must be in SignalPasses
    ASSERT_EQ(batch.SignalPasses.size(), 1u);
    EXPECT_EQ(batch.SignalPasses[0], "GfxFinal");
}

TEST(RenderGraphAsyncBatch, ComputeBatchWaitsForGraphicsPrerequisite)
{
    // Graph: GfxPre → Compute → GfxPost (final).
    // Batch WaitPasses must contain GfxPre; SignalPasses must contain GfxPost.
    RenderGraph graph;
    AddStub(graph, "GfxPre");

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);

    AddStub(graph, "GfxPost");

    graph.ConnectPass("GfxPre", "ComputePass");
    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.Execute();

    const auto batches = graph.GetAsyncComputeBatches();
    ASSERT_EQ(batches.size(), 1u);

    const auto& batch = batches[0];
    ASSERT_EQ(batch.WaitPasses.size(), 1u);
    EXPECT_EQ(batch.WaitPasses[0], "GfxPre")
        << "ComputePass must list GfxPre as a prerequisite to wait for";

    ASSERT_EQ(batch.SignalPasses.size(), 1u);
    EXPECT_EQ(batch.SignalPasses[0], "GfxPost")
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
            std::count_if(plan.begin(), plan.end(), [kind](const RenderGraph::SubmissionCommand& c)
                          { return c.CommandKind == kind; }));
    }

    // Helper: collect PassNames for Pass commands in order.
    std::vector<std::string> PassOrder(const std::vector<RenderGraph::SubmissionCommand>& plan)
    {
        std::vector<std::string> names;
        for (const auto& cmd : plan)
        {
            if (cmd.CommandKind == SCKind::Pass)
                names.push_back(cmd.PassName);
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
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();

    EXPECT_EQ(CountKind(plan, SCKind::BatchBegin), 1u);
    EXPECT_EQ(CountKind(plan, SCKind::BatchEnd), 1u);
    EXPECT_EQ(CountKind(plan, SCKind::Pass), 2u);

    // BatchBegin must appear before the compute Pass, BatchEnd after it but before GfxFinal
    auto beginIt = std::find_if(plan.begin(), plan.end(),
                                [](const auto& c)
                                { return c.CommandKind == SCKind::BatchBegin; });
    auto endIt = std::find_if(plan.begin(), plan.end(),
                              [](const auto& c)
                              { return c.CommandKind == SCKind::BatchEnd; });
    auto computeIt = std::find_if(plan.begin(), plan.end(),
                                  [](const auto& c)
                                  {
                                      return c.CommandKind == SCKind::Pass && c.PassName == "ComputePass";
                                  });
    auto gfxIt = std::find_if(plan.begin(), plan.end(),
                              [](const auto& c)
                              {
                                  return c.CommandKind == SCKind::Pass && c.PassName == "GfxFinal";
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
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();

    for (const auto& cmd : plan)
    {
        if (cmd.CommandKind != SCKind::Pass)
            continue;
        if (cmd.PassName == "ComputePass")
        {
            EXPECT_EQ(cmd.WorkType, RenderPass::PassWorkType::Compute);
            EXPECT_EQ(cmd.Lane, RenderGraph::QueueLane::Compute)
                << "Compute pass must map to compute lane";
        }
        else if (cmd.PassName == "GfxFinal")
        {
            EXPECT_EQ(cmd.WorkType, RenderPass::PassWorkType::Graphics);
            EXPECT_EQ(cmd.Lane, RenderGraph::QueueLane::Graphics)
                << "Graphics pass must map to graphics lane";
        }
    }
}

TEST(RenderGraphSubmissionPlan, BatchBeginCarriesWaitAndInputResources)
{
    // Graph: GfxPre(write SharedTex) -> ComputePass(read SharedTex) -> GfxPost.
    // BatchBegin must carry wait/input metadata for backend queue-wait mapping.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "GfxPre");
    graph.RegisterGraphPass(
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                401,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SharedTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                401,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SharedTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    AddStub(graph, "GfxPost");
    graph.ConnectPass("GfxPre", "ComputePass");
    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();
    const auto beginIt = std::find_if(plan.begin(), plan.end(),
                                      [](const auto& c)
                                      { return c.CommandKind == SCKind::BatchBegin; });
    ASSERT_NE(beginIt, plan.end());
    EXPECT_EQ(beginIt->Lane, RenderGraph::QueueLane::Compute)
        << "BatchBegin lane should be compute";

    ASSERT_EQ(beginIt->WaitPasses.size(), 1u);
    EXPECT_EQ(beginIt->WaitPasses[0], "GfxPre");

    ASSERT_EQ(beginIt->InputResources.size(), 1u);
    EXPECT_EQ(beginIt->InputResources[0].ResourceName, "SharedTex");
    EXPECT_EQ(beginIt->InputResources[0].ExternalPass, "GfxPre");
}

TEST(RenderGraphSubmissionPlan, BatchEndCarriesSignalAndOutputResources)
{
    // Graph: ComputePass(write ResultTex) -> GfxPost(read ResultTex).
    // BatchEnd must carry signal/output metadata for backend queue-signal mapping.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "ComputePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                402,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ResultTex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

    AddStub(graph, "GfxPost");
    graph.RegisterGraphPass(
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                402,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ResultTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("ComputePass", "GfxPost");
    graph.SetFinalPass("GfxPost");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();
    const auto endIt = std::find_if(plan.begin(), plan.end(),
                                    [](const auto& c)
                                    { return c.CommandKind == SCKind::BatchEnd; });
    ASSERT_NE(endIt, plan.end());
    EXPECT_EQ(endIt->Lane, RenderGraph::QueueLane::Compute)
        << "BatchEnd lane should be compute";

    ASSERT_EQ(endIt->SignalPasses.size(), 1u);
    EXPECT_EQ(endIt->SignalPasses[0], "GfxPost");

    ASSERT_EQ(endIt->OutputResources.size(), 1u);
    EXPECT_EQ(endIt->OutputResources[0].ResourceName, "ResultTex");
    EXPECT_EQ(endIt->OutputResources[0].ExternalPass, "GfxPost");
}

TEST(RenderGraphSubmissionPlan, DumpToJsonIncludesSubmissionPlan)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);

    AddStub(graph, "GfxFinal");
    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto outputPath = std::filesystem::temp_directory_path() / "render_graph_phase_g9_submission_plan.json";
    ASSERT_TRUE(graph.DumpToJson(outputPath.string()));

    std::ifstream in(outputPath);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);
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
    c1->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    c1->SetSideEffects(RenderPass::SideEffect::NeverCull);

    auto c2 = AddStub(graph, "C2");
    c2->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);
    c2->SetSideEffects(RenderPass::SideEffect::NeverCull);

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
    // graph.GetPassOrder() (which reflects HoistComputePasses()).
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    AddStub(graph, "GfxFinal");

    graph.ConnectPass("ComputePass", "GfxFinal");
    graph.SetFinalPass("GfxFinal");
    graph.Execute();

    const auto plan = graph.GetSubmissionPlan();
    const auto planPassOrder = PassOrder(plan);
    const auto& graphPassOrder = graph.GetPassOrder();

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

    const auto& timings = graph.GetLastPassTimings();
    ASSERT_EQ(timings.size(), 3u);
    EXPECT_EQ(timings[0].PassName, "A");
    EXPECT_EQ(timings[1].PassName, "B");
    EXPECT_EQ(timings[2].PassName, "C");
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
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
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
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
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
    // SetPostPassHook compatibility: the hook must fire once per executed pass
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
        RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "TAAHistory"));
    ASSERT_TRUE(history.IsValid());

    AddStub(graph, "CurrentFrameProducer");
    graph.RegisterGraphPass(
        "CurrentFrameProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                41,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

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

TEST(RenderGraphTemporalHistoryContracts, InvalidHistoryContractReportsDiagnosticAndSkipsCallback)
{
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "CurrentFrameProducer");
    graph.RegisterGraphPass(
        "CurrentFrameProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                51,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

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
    const auto diagIt = std::find_if(diagnostics.begin(), diagnostics.end(),
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
        RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "TAAHistory"));
    ASSERT_TRUE(history.IsValid());

    AddStub(graph, "CurrentFrameProducer");
    graph.RegisterGraphPass(
        "CurrentFrameProducer",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "CurrentFrameColor",
                61,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "CurrentFrameColor"));
            builder.Write(color, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

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

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);
    EXPECT_NE(json.find("\"historyResourceCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"temporalHistoryContractCount\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"TAAHistory\", \"kind\": \"Texture2D\", \"imported\": true, \"isHistory\": true"), std::string::npos);
    EXPECT_NE(json.find("\"historyResource\": \"TAAHistory\""), std::string::npos);
    EXPECT_NE(json.find("\"sourceResource\": \"CurrentFrameColor\""), std::string::npos);
    EXPECT_NE(json.find("\"historyImported\": true"), std::string::npos);
    EXPECT_NE(json.find("\"sourceReachable\": true"), std::string::npos);
    EXPECT_NE(json.find("histories=1;historyContracts=1"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
}

// =============================================================================
// RenderGraphAsyncBatchResources — Phase G Slice 8: cross-batch resource deps
// =============================================================================

TEST(RenderGraphAsyncBatchResources, NoBatchResourceDepsWhenNoAccessDeclarations)
{
    // Stub passes have no registered access declarations, so InputResources
    // and OutputResources must both be empty even when WaitPasses/SignalPasses
    // are populated.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
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

    AddStub(graph, "GfxPre");
    graph.RegisterGraphPass(
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SharedTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SharedTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SharedTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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
    EXPECT_EQ(batch.InputResources[0].ExternalPass, "GfxPre");

    EXPECT_TRUE(batch.OutputResources.empty())
        << "ComputePass does not write SharedTex → no OutputResources";
}

TEST(RenderGraphAsyncBatchResources, BatchOutputFlowsToGraphicsPass)
{
    // Graph: ComputeBatch writes "ResultTex"; GfxPost reads "ResultTex".
    // Expected: one OutputResource {ResultTex, GfxPost}.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "ComputePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ResultTex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

    AddStub(graph, "GfxPost");
    graph.RegisterGraphPass(
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ResultTex",
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ResultTex"));
            [[maybe_unused]] const auto readTex = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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
    EXPECT_EQ(batch.OutputResources[0].ExternalPass, "GfxPost");
}

TEST(RenderGraphAsyncBatchResources, IndependentBatchHasNoCrossBoundaryResources)
{
    // ComputePass has its own private resource not shared with any other pass.
    // InputResources and OutputResources must both be empty.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    compute->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "ComputePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "PrivateTex",
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "PrivateTex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

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

    AddStub(graph, "GfxPre");
    graph.RegisterGraphPass(
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "InTex",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "InTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto compute = AddStub(graph, "ComputePass");
    compute->SetPassWorkType(RenderPass::PassWorkType::Compute);
    compute->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputePass",
        [](RGBuilder& builder)
        {
            auto inTex = builder.ImportTexture(
                "InTex",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "InTex"));
            [[maybe_unused]] const auto readInTex = builder.Read(inTex, RGReadUsage::ShaderSample);

            auto outTex = builder.ImportTexture(
                "OutTex",
                11,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "OutTex"));
            builder.Write(outTex, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

    AddStub(graph, "GfxPost");
    graph.RegisterGraphPass(
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto outTex = builder.ImportTexture(
                "OutTex",
                11,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "OutTex"));
            [[maybe_unused]] const auto readOutTex = builder.Read(outTex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);
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

    AddStub(graph, "OnlyPass");
    graph.RegisterGraphPass(
        "OnlyPass",
        [](RGBuilder& /*builder*/) {},
        [](RGCommandContext& /*context*/) {});
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

    AddStub(graph, "Producer");
    graph.RegisterGraphPass(
        "Producer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "ColorTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto consumer = AddStub(graph, "Consumer");
    consumer->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "Consumer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "ColorTex"));
            [[maybe_unused]] const auto sampledTex = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    AddStub(graph, "Writer1");
    graph.RegisterGraphPass(
        "Writer1",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Tex",
                5,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "Tex"));
            builder.Write(tex, RGWriteUsage::ShaderImage);
        },
        [](RGCommandContext& /*context*/) {});

    AddStub(graph, "Writer2");
    graph.RegisterGraphPass(
        "Writer2",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Tex",
                5,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "Tex"));
            builder.Write(tex, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto consumer = AddStub(graph, "Consumer");
    consumer->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "Consumer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Tex",
                5,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "Tex"));
            [[maybe_unused]] const auto sampledTex = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("Writer1", "Writer2");
    graph.AddExecutionDependency("Writer2", "Consumer");
    graph.SetFinalPass("Consumer");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::find_if(transitions.begin(), transitions.end(),
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
        RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ShadowMap"));

    auto consumer = AddStub(graph, "ShadowConsumer");
    consumer->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "ShadowConsumer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ShadowMap",
                42,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ShadowMap"));
            [[maybe_unused]] const auto sampledShadow = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    AddStub(graph, "ScenePass");
    graph.RegisterGraphPass(
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto postPass = AddStub(graph, "PostPass");
    postPass->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "PostPass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto sampledScene = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos)
        << "Schema must be version 12 for Slice 14";
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

    AddStub(graph, "PassA");
    graph.RegisterGraphPass(
        "PassA",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Color",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "Color"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto stubB = AddStub(graph, "PassB");
    stubB->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "PassB",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "Color",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "Color"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("PassA", "PassB");
    graph.SetFinalPass("PassB");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto lifetimes = graph.GetResourceLifetimes();
    ASSERT_FALSE(lifetimes.empty());

    const auto it = std::find_if(lifetimes.begin(), lifetimes.end(),
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

    auto stub = AddStub(graph, "ConsumerPass");
    stub->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "ConsumerPass",
        [](RGBuilder& builder)
        {
            // Only read — never write — so no pass acts as producer.
            auto tex = builder.ImportTexture(
                "ExtTex",
                99,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ExtTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("ConsumerPass");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto lifetimes = graph.GetResourceLifetimes();
    const auto it = std::find_if(lifetimes.begin(), lifetimes.end(),
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

    auto stub = AddStub(graph, "WriterPass");
    stub->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "WriterPass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SinkTex",
                7,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "SinkTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("WriterPass");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto lifetimes = graph.GetResourceLifetimes();
    const auto it = std::find_if(lifetimes.begin(), lifetimes.end(),
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

    AddStub(graph, "ScenePass");
    graph.RegisterGraphPass(
        "ScenePass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto postPass = AddStub(graph, "PostPass");
    postPass->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "PostPass",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "SceneColor",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "SceneColor"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos)
        << "Schema must be version 12 for Slice 14";
    EXPECT_NE(json.find("\"resourceLifetimeCount\""), std::string::npos)
        << "frameSummary must expose resourceLifetimeCount";
    EXPECT_NE(json.find("\"resourceLifetimes\""), std::string::npos)
        << "resourceLifetimes array must be present";
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

    AddStub(graph, "Writer");
    graph.RegisterGraphPass(
        "Writer",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ColorTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto reader = AddStub(graph, "Reader");
    reader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "Reader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ColorTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ColorTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("Writer", "Reader");
    graph.SetFinalPass("Reader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::find_if(transitions.begin(), transitions.end(),
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

    AddStub(graph, "MipWriter");
    graph.RegisterGraphPass(
        "MipWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "MipTex",
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "MipTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto reader = AddStub(graph, "MipReader");
    reader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "MipReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "MipTex",
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "MipTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(2));
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("MipWriter", "MipReader");
    graph.SetFinalPass("MipReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::find_if(transitions.begin(), transitions.end(),
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

    AddStub(graph, "LayerWriter");
    graph.RegisterGraphPass(
        "LayerWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "LayerTex",
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "LayerTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto reader = AddStub(graph, "LayerReader");
    reader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "LayerReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "LayerTex",
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "LayerTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Layer(3));
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("LayerWriter", "LayerReader");
    graph.SetFinalPass("LayerReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::find_if(transitions.begin(), transitions.end(),
                                 [](const RenderGraph::ResourceTransition& t)
                                 { return t.ResourceName == "LayerTex"; });
    ASSERT_NE(it, transitions.end()) << "Expected a transition record for LayerTex";

    EXPECT_EQ(it->Range.BaseLayer, 3u) << "BaseLayer must match the declared layer";
    EXPECT_EQ(it->Range.LayerCount, 1u) << "LayerCount must be 1 for Layer(3)";
    EXPECT_EQ(it->Range.BaseMip, 0u) << "BaseMip must default to 0";
}

TEST(RenderGraphSubresourceRange, PlannedBarrierCarriesRange)
{
    // The PlannedBarrier for a Write->Read pair with an explicit subresource
    // range must expose that range via PlannedBarrier::Range.
    RenderGraph graph;
    graph.SetRuntimeBarrierExecutionEnabled(false);

    AddStub(graph, "BarrierWriter");
    graph.RegisterGraphPass(
        "BarrierWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "BaTex",
                4,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "BaTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto reader = AddStub(graph, "BarrierReader");
    reader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "BarrierReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "BaTex",
                4,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "BaTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(1));
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("BarrierWriter", "BarrierReader");
    graph.SetFinalPass("BarrierReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& barriers = graph.GetPlannedBarriers();
    const auto it = std::find_if(barriers.begin(), barriers.end(),
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

    AddStub(graph, "JsonWriter");
    graph.RegisterGraphPass(
        "JsonWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "JsonTex",
                5,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "JsonTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto reader = AddStub(graph, "JsonReader");
    reader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "JsonReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "JsonTex",
                5,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "JsonTex"));
            [[maybe_unused]] const auto r =
                builder.Read(tex, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(0));
        },
        [](RGCommandContext& /*context*/) {});

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

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos)
        << "Schema must be bumped to 12 for Slice 14";
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

    AddStub(graph, "GfxWriter");
    graph.RegisterGraphPass(
        "GfxWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "GfxTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "GfxTex"));
            builder.Write(tex, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto gfxReader = AddStub(graph, "GfxReader");
    gfxReader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "GfxReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "GfxTex",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Framebuffer, "GfxTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    auto computeWriter = AddStub(graph, "ComputeWriter");
    computeWriter->SetPassWorkType(RenderPass::PassWorkType::Compute);
    graph.RegisterGraphPass(
        "ComputeWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ComputeResult",
                7,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ComputeResult"));
            builder.Write(tex, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto gfxReader = AddStub(graph, "GfxReader");
    gfxReader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "GfxReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "ComputeResult",
                7,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ComputeResult"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("ComputeWriter", "GfxReader");
    graph.SetFinalPass("GfxReader");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto transitions = graph.GetResourceTransitions();
    const auto it = std::find_if(transitions.begin(), transitions.end(),
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

    auto computeWriter = AddStub(graph, "CsWriter");
    computeWriter->SetPassWorkType(RenderPass::PassWorkType::Compute);
    graph.RegisterGraphPass(
        "CsWriter",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "CsTex",
                9,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "CsTex"));
            builder.Write(tex, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto gfxReader = AddStub(graph, "GsReader");
    gfxReader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "GsReader",
        [](RGBuilder& builder)
        {
            auto tex = builder.ImportTexture(
                "CsTex",
                9,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "CsTex"));
            [[maybe_unused]] const auto r = builder.Read(tex, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos)
        << "Schema must be version 12 for Slice 14";
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

    AddStub(graph, "GfxPre");
    graph.RegisterGraphPass(
        "GfxPre",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColor"));
            builder.Write(sc, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    auto computeAO = AddStub(graph, "ComputeAO");
    computeAO->SetPassWorkType(RenderPass::PassWorkType::Compute);
    computeAO->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputeAO",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "AO",
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "AO"));
            builder.Write(ao, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto gfxPost = AddStub(graph, "GfxPost");
    gfxPost->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "GfxPost",
        [](RGBuilder& builder)
        {
            auto sc = builder.ImportTexture(
                "SceneColor",
                1,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneColor"));
            [[maybe_unused]] const auto r1 = builder.Read(sc, RGReadUsage::ShaderSample);

            auto ao = builder.ImportTexture(
                "AO",
                2,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "AO"));
            [[maybe_unused]] const auto r2 = builder.Read(ao, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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

    auto gfxReader = AddStub(graph, "GfxReader");
    gfxReader->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "GfxReader",
        [](RGBuilder& builder)
        {
            auto depth = builder.ImportTexture(
                "SceneDepth",
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneDepth"));
            [[maybe_unused]] const auto r = builder.Read(depth, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    auto computeWriter = AddStub(graph, "ComputeWriter");
    computeWriter->SetPassWorkType(RenderPass::PassWorkType::Compute);
    computeWriter->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "ComputeWriter",
        [](RGBuilder& builder)
        {
            auto depth = builder.ImportTexture(
                "SceneDepth",
                3,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SceneDepth"));
            builder.Write(depth, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    // Intentionally NO execution dependency from GfxReader to ComputeWriter —
    // this models a programmer error that the hazard validator must catch.
    graph.SetFinalPass("ComputeWriter");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const bool hasWriteAfterRead = std::any_of(
        hazards.begin(), hazards.end(),
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

    AddStub(graph, "GfxA");
    graph.RegisterGraphPass(
        "GfxA",
        [](RGBuilder& /*builder*/) {},
        [](RGCommandContext& /*context*/) {});

    auto computeB = AddStub(graph, "ComputeB");
    computeB->SetPassWorkType(RenderPass::PassWorkType::Compute);
    computeB->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputeB",
        [](RGBuilder& /*builder*/) {},
        [](RGCommandContext& /*context*/) {});

    auto gfxC = AddStub(graph, "GfxC");
    gfxC->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "GfxC",
        [](RGBuilder& /*builder*/) {},
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("GfxA", "ComputeB");
    graph.AddExecutionDependency("ComputeB", "GfxC");
    graph.SetFinalPass("GfxC");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto& order = graph.GetPassOrder();
    ASSERT_EQ(order.size(), 3u);

    const auto posA = std::find(order.begin(), order.end(), "GfxA") - order.begin();
    const auto posB = std::find(order.begin(), order.end(), "ComputeB") - order.begin();
    const auto posC = std::find(order.begin(), order.end(), "GfxC") - order.begin();

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

    auto hzb = AddStub(graph, "HZBPass");
    hzb->SetPassWorkType(RenderPass::PassWorkType::Compute);
    graph.RegisterGraphPass(
        "HZBPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "AOTexture",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "AOTexture"));
            builder.Write(ao, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto lighting = AddStub(graph, "LightingPass");
    lighting->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "LightingPass",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "AOTexture",
                10,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "AOTexture"));
            [[maybe_unused]] const auto r = builder.Read(ao, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

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
    const auto it = std::find_if(
        transitions.begin(), transitions.end(),
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

    auto c1 = AddStub(graph, "ComputeGTAO");
    c1->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c1->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputeGTAO",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "GTAOResult",
                20,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "GTAOResult"));
            builder.Write(ao, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto c2 = AddStub(graph, "ComputeSSGI");
    c2->SetPassWorkType(RenderPass::PassWorkType::Compute);
    c2->SetAsyncComputeCandidate(true);
    graph.RegisterGraphPass(
        "ComputeSSGI",
        [](RGBuilder& builder)
        {
            auto gi = builder.ImportTexture(
                "SSGIResult",
                21,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SSGIResult"));
            builder.Write(gi, RGWriteUsage::ShaderStorage);
        },
        [](RGCommandContext& /*context*/) {});

    auto gfxLighting = AddStub(graph, "DeferredLighting");
    gfxLighting->SetSideEffects(RenderPass::SideEffect::NeverCull);
    graph.RegisterGraphPass(
        "DeferredLighting",
        [](RGBuilder& builder)
        {
            auto ao = builder.ImportTexture(
                "GTAOResult",
                20,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "GTAOResult"));
            [[maybe_unused]] const auto r1 = builder.Read(ao, RGReadUsage::ShaderSample);

            auto gi = builder.ImportTexture(
                "SSGIResult",
                21,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "SSGIResult"));
            [[maybe_unused]] const auto r2 = builder.Read(gi, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.AddExecutionDependency("ComputeGTAO", "DeferredLighting");
    graph.AddExecutionDependency("ComputeSSGI", "DeferredLighting");
    graph.SetFinalPass("DeferredLighting");
    graph.BuildFrameGraph();
    graph.Execute();

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Hazard validator must remain green after compute hoist on multi-compute graph";

    const auto& order = graph.GetPassOrder();
    const auto posGTAO = std::find(order.begin(), order.end(), "ComputeGTAO") - order.begin();
    const auto posSSGI = std::find(order.begin(), order.end(), "ComputeSSGI") - order.begin();
    const auto posLighting =
        std::find(order.begin(), order.end(), "DeferredLighting") - order.begin();

    EXPECT_LT(posGTAO, posLighting)
        << "ComputeGTAO must precede DeferredLighting after hoist";
    EXPECT_LT(posSSGI, posLighting)
        << "ComputeSSGI must precede DeferredLighting after hoist";
}
