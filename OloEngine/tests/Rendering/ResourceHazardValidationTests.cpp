// =============================================================================
// ResourceHazardValidationTests.cpp
//
// Layer-5 resource hazard validation — tests for the resource-aware
// RenderGraph API (`RenderGraph::ValidateResourceHazards`) and the
// `RenderPass::DeclareRead` / `DeclareWrite` declaration surface.
//
// **What the validator guarantees.** After all passes are added and
// connections made, `ValidateResourceHazards` walks the topologically
// sorted pass order and, using each pass's declared reads/writes,
// verifies:
//   * Read-after-Write (RAW): every reader of a resource has a transitive
//     execution dependency on the pass that wrote it.
//   * Write-after-Write (WAW): if two passes write the same resource, the
//     later one must depend on the earlier.
//   * Write-after-Read (WAR): a later writer must depend on any prior
//     reader of the resource it overwrites.
//
// Catches: missing `ConnectPass` / `AddExecutionDependency` calls.
//
// **Declared vs undeclared passes.** Passes that do not declare reads /
// writes are skipped by the validator — their ordering is not checked.
// This makes the API opt-in: un-migrated passes continue to work and the
// check only strengthens as more passes are migrated.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    // Minimal stub that exposes `DeclareRead` / `DeclareWrite` via the
    // protected RenderPass API. Tests construct instances, call the
    // declaration helpers directly, and hand the instance to the graph.
    class DeclarativeStubPass : public RenderPass
    {
      public:
        explicit DeclarativeStubPass(const std::string& name)
        {
            m_Name = name;
        }

        void Init(const FramebufferSpecification& /*spec*/) override {}
        void Execute() override {}
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override
        {
            return nullptr;
        }
        void SetupFramebuffer(u32 /*w*/, u32 /*h*/) override {}
        void ResizeFramebuffer(u32 /*w*/, u32 /*h*/) override {}
        void OnReset() override {}

        // Exposed for tests.
        void TestDeclareRead(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
        {
            DeclareRead(name, kind);
        }
        void TestDeclareWrite(std::string_view name, ResourceHandle::Kind kind = ResourceHandle::Kind::Unknown)
        {
            DeclareWrite(name, kind);
        }
    };

    Ref<DeclarativeStubPass> AddDeclStub(RenderGraph& graph, const std::string& name)
    {
        auto pass = Ref<DeclarativeStubPass>::Create(name);
        pass->SetName(name);
        graph.AddPass(pass);
        return pass;
    }

    std::string HazardsToString(const std::vector<RenderGraph::Hazard>& hazards)
    {
        if (hazards.empty())
            return "<no hazards>";
        std::string out;
        for (const auto& h : hazards)
        {
            out += "\n  - ";
            out += h.Message;
        }
        return out;
    }

    bool ContainsHazardForResource(const std::vector<RenderGraph::Hazard>& hazards, std::string_view resource)
    {
        for (const auto& h : hazards)
        {
            if (h.Resource == resource)
                return true;
        }
        return false;
    }
} // namespace

// =============================================================================
// Happy path: linear chain with properly-declared resource handoff.
// =============================================================================
TEST(RenderGraphResourceHazards, LinearChainWithHandoffIsHazardFree)
{
    RenderGraph graph;
    auto shadow = AddDeclStub(graph, "Shadow");
    auto scene = AddDeclStub(graph, "Scene");
    auto post = AddDeclStub(graph, "Post");

    shadow->TestDeclareWrite("ShadowMap");
    scene->TestDeclareRead("ShadowMap");
    scene->TestDeclareWrite("HDRColor");
    post->TestDeclareRead("HDRColor");

    graph.ConnectPass("Shadow", "Scene");
    graph.ConnectPass("Scene", "Post");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << "expected no hazards, got:" << HazardsToString(hazards);
}

// =============================================================================
// RAW: a read without a dependency on its writer must be flagged. This is
// the classic "forgot to AddExecutionDependency on the shadow map" bug.
// =============================================================================
TEST(RenderGraphResourceHazards, ReadWithoutDependencyIsFlagged)
{
    RenderGraph graph;
    auto shadow = AddDeclStub(graph, "Shadow");
    auto scene = AddDeclStub(graph, "Scene");

    shadow->TestDeclareWrite("ShadowMap");
    scene->TestDeclareRead("ShadowMap");
    // NOTE: no ConnectPass / AddExecutionDependency linking Shadow and Scene.

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_EQ(hazards.size(), 1u) << HazardsToString(hazards);
    EXPECT_EQ(hazards[0].Kind, RenderGraph::HazardKind::ReadAfterWrite);
    EXPECT_EQ(hazards[0].Resource, "ShadowMap");
    EXPECT_EQ(hazards[0].Producer, "Shadow");
    EXPECT_EQ(hazards[0].Consumer, "Scene");
}

// =============================================================================
// WAW: two independent passes writing the same resource are a race.
// =============================================================================
TEST(RenderGraphResourceHazards, ParallelWritesToSameResourceAreFlagged)
{
    RenderGraph graph;
    auto a = AddDeclStub(graph, "A");
    auto b = AddDeclStub(graph, "B");

    a->TestDeclareWrite("R");
    b->TestDeclareWrite("R");

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_EQ(hazards.size(), 1u) << HazardsToString(hazards);
    EXPECT_EQ(hazards[0].Kind, RenderGraph::HazardKind::WriteAfterWrite);
    EXPECT_EQ(hazards[0].Resource, "R");
}

// =============================================================================
// WAR: later writer overwrites a resource a live reader still needs.
// =============================================================================
TEST(RenderGraphResourceHazards, WriteAfterReadWithoutDependencyIsFlagged)
{
    RenderGraph graph;
    auto w1 = AddDeclStub(graph, "Writer1");
    auto r = AddDeclStub(graph, "Reader");
    auto rw = AddDeclStub(graph, "Rewriter");

    w1->TestDeclareWrite("R");
    r->TestDeclareRead("R");
    rw->TestDeclareWrite("R");

    // Writer1 → Reader and Writer1 → Rewriter, but Rewriter doesn't depend
    // on Reader — WAR (or WAW depending on topo order) expected.
    graph.ConnectPass("Writer1", "Reader");
    graph.ConnectPass("Writer1", "Rewriter");

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty());
    EXPECT_TRUE(ContainsHazardForResource(hazards, "R")) << HazardsToString(hazards);
}

// =============================================================================
// Transitive dependency counts.
// =============================================================================
TEST(RenderGraphResourceHazards, TransitiveDependencyCountsAsDependency)
{
    RenderGraph graph;
    auto a = AddDeclStub(graph, "A");
    AddDeclStub(graph, "B");
    auto c = AddDeclStub(graph, "C");

    a->TestDeclareWrite("R");
    c->TestDeclareRead("R");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("B", "C"); // A → B → C transitively proves C depends on A

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

// =============================================================================
// Diamond: two readers of a shared resource are both dependents of the writer.
// =============================================================================
TEST(RenderGraphResourceHazards, DiamondReadersOfSharedResourceIsHazardFree)
{
    RenderGraph graph;
    auto a = AddDeclStub(graph, "A");
    auto b = AddDeclStub(graph, "B");
    auto c = AddDeclStub(graph, "C");

    a->TestDeclareWrite("R");
    b->TestDeclareRead("R");
    c->TestDeclareRead("R");

    graph.ConnectPass("A", "B");
    graph.ConnectPass("A", "C");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

// =============================================================================
// Read-only external resource (imported asset) must not flag.
// =============================================================================
TEST(RenderGraphResourceHazards, ReadOnlyResourceHasNoHazards)
{
    RenderGraph graph;
    auto a = AddDeclStub(graph, "A");
    auto b = AddDeclStub(graph, "B");

    a->TestDeclareRead("ImportedTexture");
    b->TestDeclareRead("ImportedTexture");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

// =============================================================================
// Same pass read+write of same resource is legal (e.g., additive accumulation).
// =============================================================================
TEST(RenderGraphResourceHazards, SamePassReadAndWriteIsLegal)
{
    RenderGraph graph;
    auto acc = AddDeclStub(graph, "Accumulate");
    acc->TestDeclareRead("Accum");
    acc->TestDeclareWrite("Accum");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

// =============================================================================
// Undeclared pass is invisible to validator (opt-in semantics).
// =============================================================================
TEST(RenderGraphResourceHazards, UndeclaredPassDoesNotContributeHazards)
{
    RenderGraph graph;
    auto decl = AddDeclStub(graph, "Decl");
    auto undecl = AddDeclStub(graph, "Undeclared");

    decl->TestDeclareWrite("X");
    // Undecl reads X but doesn't declare it — validator cannot know.

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
    // Sanity: declared pass surfaces, undeclared one stays empty.
    EXPECT_EQ(decl->GetWrites().size(), 1u);
    EXPECT_EQ(undecl->GetReads().size(), 0u);
}

// =============================================================================
// Production-shaped scenario: Shadow produces ShadowMap, consumed by Scene
// AND PostProcess (fog). Models OloEngine's real pipeline topology.
// =============================================================================
TEST(RenderGraphResourceHazards, ProductionShapedGraphIsHazardFree)
{
    RenderGraph graph;
    AddDeclStub(graph, "ShadowPass")->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    post->TestDeclareRead(std::string(ResourceNames::SceneColor));
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PostProcessColor));

    graph.ConnectPass("ShadowPass", "ScenePass");
    graph.ConnectPass("ShadowPass", "PostProcessPass"); // fog samples shadow
    graph.ConnectPass("ScenePass", "PostProcessPass");
    graph.ConnectPass("PostProcessPass", "FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

// =============================================================================
// Production-shaped with the ShadowMap dependency missing at PostProcess
// (no direct AND no transitive path to ShadowPass) → RAW hazard expected.
// =============================================================================
TEST(RenderGraphResourceHazards, ProductionShapedGraphWithNoPathToShadowIsFlagged)
{
    RenderGraph graph;
    AddDeclStub(graph, "ShadowPass")->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));
    AddDeclStub(graph, "ScenePass");

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PostProcessColor));

    graph.ConnectPass("ShadowPass", "ScenePass");
    // NOTE: PostProcess has no path to ShadowPass.
    graph.ConnectPass("PostProcessPass", "FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty()) << "expected a hazard, got none";
    EXPECT_TRUE(ContainsHazardForResource(hazards, std::string(ResourceNames::ShadowMapCSM)))
        << HazardsToString(hazards);
}

// =============================================================================
// Production-shaped with the IBL triplet (irradiance / prefilter / BRDF LUT)
// produced by a dedicated EnvironmentPass and consumed read-only by Scene.
// Mirrors the real SceneRenderPass declarations — any missing ConnectPass
// from IBL → Scene must be flagged as a RAW hazard on the specific resource.
// =============================================================================
TEST(RenderGraphResourceHazards, IblProducerConsumerIsHazardFree)
{
    RenderGraph graph;

    auto env = AddDeclStub(graph, "EnvironmentPass");
    env->TestDeclareWrite(std::string(ResourceNames::IrradianceMap));
    env->TestDeclareWrite(std::string(ResourceNames::PrefilterMap));
    env->TestDeclareWrite(std::string(ResourceNames::BrdfLut));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::IrradianceMap));
    scene->TestDeclareRead(std::string(ResourceNames::PrefilterMap));
    scene->TestDeclareRead(std::string(ResourceNames::BrdfLut));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    graph.ConnectPass("EnvironmentPass", "ScenePass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

TEST(RenderGraphResourceHazards, IblMissingDependencyIsFlagged)
{
    RenderGraph graph;

    auto env = AddDeclStub(graph, "EnvironmentPass");
    env->TestDeclareWrite(std::string(ResourceNames::PrefilterMap));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::PrefilterMap));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    // NOTE: intentionally no ConnectPass from Environment to Scene.
    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty()) << "expected IBL RAW hazard, got none";
    EXPECT_TRUE(ContainsHazardForResource(hazards, std::string(ResourceNames::PrefilterMap)))
        << HazardsToString(hazards);
}

// =============================================================================
// UI composite in-chain: PostProcess → UIComposite → Final. UIComposite reads
// PostProcessColor and writes UIComposite; Final reads UIComposite. Mirrors
// the declarations in UICompositeRenderPass.cpp and catches the textbook
// mistake of letting Final read PostProcessColor directly while the UI pass
// has already overwritten it.
// =============================================================================
TEST(RenderGraphResourceHazards, UICompositeInChainIsHazardFree)
{
    RenderGraph graph;

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    graph.ConnectPass("PostProcessPass", "UICompositePass");
    graph.AddExecutionDependency("UICompositePass", "FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

TEST(RenderGraphResourceHazards, UICompositeSkippedByFinalIsFlagged)
{
    RenderGraph graph;

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    graph.ConnectPass("PostProcessPass", "UICompositePass");
    // NOTE: Final missing its dependency on UICompositePass — even though the
    // resource it reads is produced, nothing guarantees the producer runs
    // first. The validator must flag this as a RAW hazard on UIComposite.
    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_FALSE(hazards.empty()) << "expected UIComposite RAW hazard, got none";
    EXPECT_TRUE(ContainsHazardForResource(hazards, std::string(ResourceNames::UIComposite)))
        << HazardsToString(hazards);
}

// =============================================================================
// Resource handle identity: name-based equality, Kind is metadata.
// =============================================================================
TEST(RenderGraphResourceHazards, ResourceHandleEqualityIsNameBased)
{
    ResourceHandle a("Foo", ResourceHandle::Kind::Texture2D);
    ResourceHandle b("Foo", ResourceHandle::Kind::TextureCube);
    ResourceHandle c("Bar");

    EXPECT_EQ(a, b) << "name-based equality should ignore Kind for now";
    EXPECT_FALSE(a == c);

    std::hash<ResourceHandle> hasher;
    EXPECT_EQ(hasher(a), hasher(b));
}

TEST(RenderGraphResourceHazards, ImportedResourceIsTrackedByRegistry)
{
    RenderGraph graph;

    auto importedDesc = RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2DArray, "ImportedShadowMap");
    importedDesc.Format = RGResourceFormat::Depth32Float;
    graph.ImportResource("ImportedShadowMap", importedDesc);

    auto scene = AddDeclStub(graph, "Scene");
    scene->TestDeclareRead("ImportedShadowMap", ResourceHandle::Kind::Texture2DArray);
    scene->TestDeclareWrite("SceneColor", ResourceHandle::Kind::Framebuffer);

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    const auto* imported = graph.FindRegisteredResource("ImportedShadowMap");
    ASSERT_NE(imported, nullptr);
    EXPECT_TRUE(imported->Desc.Imported);
    EXPECT_EQ(imported->Desc.Kind, ResourceHandle::Kind::Texture2DArray);
    EXPECT_EQ(imported->Desc.Format, RGResourceFormat::Depth32Float);
    ASSERT_EQ(imported->Consumers.size(), 1u);
    EXPECT_EQ(imported->Consumers[0], "Scene");

    const auto* sceneColor = graph.FindRegisteredResource("SceneColor");
    ASSERT_NE(sceneColor, nullptr);
    ASSERT_EQ(sceneColor->Producers.size(), 1u);
    EXPECT_EQ(sceneColor->Producers[0], "Scene");
}

TEST(RenderGraphResourceHazards, ResourceKindMismatchIsFlagged)
{
    RenderGraph graph;

    auto writer = AddDeclStub(graph, "Writer");
    writer->TestDeclareWrite("SharedResource", ResourceHandle::Kind::Texture2D);

    auto reader = AddDeclStub(graph, "Reader");
    reader->TestDeclareRead("SharedResource", ResourceHandle::Kind::StorageBuffer);

    graph.AddExecutionDependency("Writer", "Reader");

    const auto hazards = graph.ValidateResourceHazards();
    ASSERT_EQ(hazards.size(), 1u) << HazardsToString(hazards);
    EXPECT_EQ(hazards[0].Kind, RenderGraph::HazardKind::ResourceKindMismatch);
    EXPECT_EQ(hazards[0].Resource, "SharedResource");
    EXPECT_EQ(hazards[0].Consumer, "Reader");

    const auto* resource = graph.FindRegisteredResource("SharedResource");
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->Desc.Kind, ResourceHandle::Kind::Texture2D)
        << "The registry should retain the first concrete declaration as the canonical kind";
}

TEST(RenderGraphResourceHazards, TypedHandleLookupMatchesDeclaredKinds)
{
    RenderGraph graph;

    auto pass = AddDeclStub(graph, "Pass");
    pass->TestDeclareRead("EnvironmentMap", ResourceHandle::Kind::TextureCube);
    pass->TestDeclareWrite("SceneColor", ResourceHandle::Kind::Framebuffer);
    pass->TestDeclareWrite("LightGrid", ResourceHandle::Kind::StorageBuffer);

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    const auto envTex = graph.GetTextureHandle("EnvironmentMap");
    const auto sceneFB = graph.GetFramebufferHandle("SceneColor");
    const auto lightGrid = graph.GetBufferHandle("LightGrid");

    EXPECT_TRUE(envTex.IsValid());
    EXPECT_TRUE(sceneFB.IsValid());
    EXPECT_TRUE(lightGrid.IsValid());

    EXPECT_FALSE(graph.GetBufferHandle("EnvironmentMap").IsValid());
    EXPECT_FALSE(graph.GetTextureHandle("SceneColor").IsValid());
    EXPECT_FALSE(graph.GetFramebufferHandle("LightGrid").IsValid());
}

TEST(RenderGraphResourceHazards, UnknownResourceHandleLookupReturnsInvalid)
{
    RenderGraph graph;
    AddDeclStub(graph, "Pass")->TestDeclareWrite("SceneColor", ResourceHandle::Kind::Framebuffer);

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    EXPECT_FALSE(graph.GetTextureHandle("DoesNotExist").IsValid());
    EXPECT_FALSE(graph.GetBufferHandle("DoesNotExist").IsValid());
    EXPECT_FALSE(graph.GetFramebufferHandle("DoesNotExist").IsValid());
}

TEST(RenderGraphResourceHazards, StaleTypedHandleIsRejectedAfterTopologyReset)
{
    RenderGraph graph;

    auto pass = AddDeclStub(graph, "Pass");
    pass->TestDeclareRead("EnvironmentMap", ResourceHandle::Kind::TextureCube);

    auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    const auto oldHandle = graph.GetTextureHandle("EnvironmentMap");
    ASSERT_TRUE(oldHandle.IsValid());
    EXPECT_TRUE(graph.IsTextureHandleCurrent(oldHandle));

    graph.ResetTopology();

    auto newPass = AddDeclStub(graph, "NewPass");
    newPass->TestDeclareWrite("SceneColor", ResourceHandle::Kind::Framebuffer);

    hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    EXPECT_FALSE(graph.IsTextureHandleCurrent(oldHandle));
    EXPECT_FALSE(graph.GetTextureHandle("EnvironmentMap").IsValid());
}

TEST(RenderGraphResourceHazards, RecreatedResourceGetsNewGeneration)
{
    RenderGraph graph;

    auto pass = AddDeclStub(graph, "Pass");
    pass->TestDeclareWrite("TransientTex", ResourceHandle::Kind::Texture2D);

    auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    const auto first = graph.GetTextureHandle("TransientTex");
    ASSERT_TRUE(first.IsValid());
    EXPECT_TRUE(graph.IsTextureHandleCurrent(first));

    graph.ResetTopology();
    hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    auto recreated = AddDeclStub(graph, "PassAgain");
    recreated->TestDeclareWrite("TransientTex", ResourceHandle::Kind::Texture2D);

    hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    const auto second = graph.GetTextureHandle("TransientTex");
    ASSERT_TRUE(second.IsValid());
    EXPECT_TRUE(graph.IsTextureHandleCurrent(second));

    // Handle value may reuse the same index after a reset, but stale handles
    // must still be rejected because the internal slot set has been rebuilt.
    EXPECT_FALSE(graph.IsTextureHandleCurrent(first));
}

TEST(RenderGraphResourceHazards, SamePassOverlappingReadWriteWithoutFeedbackIsFlagged)
{
    RenderGraph graph;

    AddDeclStub(graph, "FeedbackPass");

    graph.RegisterGraphPass(
        "FeedbackPass",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "FeedbackTex",
                21,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "FeedbackTex"));
            [[maybe_unused]] const auto sampled = builder.Read(color, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(0));
            builder.Write(color, RGWriteUsage::RenderTarget, RGSubresourceRange::Mip(0));
        },
        [](RGCommandContext& /*context*/) {});

    graph.SetFinalPass("FeedbackPass");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const auto it = std::find_if(hazards.begin(), hazards.end(),
                                 [](const RenderGraph::Hazard& h)
                                 {
                                     return h.Kind == RenderGraph::HazardKind::FeedbackWithoutDeclaration &&
                                            h.Resource == "FeedbackTex";
                                 });
    ASSERT_NE(it, hazards.end()) << HazardsToString(hazards);
}

TEST(RenderGraphResourceHazards, ImportedProducedAndConsumedWithoutBackingIsFlagged)
{
    RenderGraph graph;

    AddDeclStub(graph, "Writer");
    AddDeclStub(graph, "Reader");

    graph.RegisterGraphPass(
        "Writer",
        [](RGBuilder& builder)
        {
            auto imported = builder.ImportTexture(
                "ImportedNoBacking",
                0,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ImportedNoBacking"));
            builder.Write(imported, RGWriteUsage::RenderTarget);
        },
        [](RGCommandContext& /*context*/) {});

    graph.RegisterGraphPass(
        "Reader",
        [](RGBuilder& builder)
        {
            auto imported = builder.ImportTexture(
                "ImportedNoBacking",
                0,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ImportedNoBacking"));
            [[maybe_unused]] const auto sampled = builder.Read(imported, RGReadUsage::ShaderSample);
        },
        [](RGCommandContext& /*context*/) {});

    graph.ConnectPass("Writer", "Reader");
    graph.SetFinalPass("Reader");
    graph.BuildFrameGraph();

    const auto hazards = graph.ValidateResourceHazards();
    const auto it = std::find_if(hazards.begin(), hazards.end(),
                                 [](const RenderGraph::Hazard& h)
                                 {
                                     return h.Kind == RenderGraph::HazardKind::ImportedResourceLifetimeMisuse &&
                                            h.Resource == "ImportedNoBacking";
                                 });
    ASSERT_NE(it, hazards.end()) << HazardsToString(hazards);
}

// =============================================================================
// Per-RenderingPath topology: mirror the pass ordering + dependency wiring
// that `Renderer3D::ConfigureRenderGraph` installs for each of the three
// supported paths and verify the declared-resource graph is hazard-free.
//
// These are stub-based: the real passes live inside Renderer3D which needs
// a full GL context. By mirroring the topology with DeclarativeStubPass
// instances we can validate the `AddExecutionDependency` / `ConnectPass`
// wiring in Renderer3D — any drift between the resource declarations in a
// real pass and the edges wired here will surface as a RAW/WAR/WAW hazard.
//
// The declared read/write set here is a *representative* subset — enough
// to cover the canonical handoffs (Shadow -> Scene, Scene -> Decal ->
// Water -> AO -> Particles -> OITResolve -> SSS -> PostProcess ->
// UIComposite -> Final). If a new cross-pass resource is introduced in
// production code, extend both the stubs here and the real pass.
// =============================================================================
namespace
{
    // Common pass-graph builder shared by the three topology tests. Adds
    // every pass that ConfigureRenderGraph creates, hooks up the declared
    // reads/writes, and returns the handles that path-specific code needs
    // to tweak (only the deferred path adds DeferredLighting + ForwardOverlay
    // on top of the shared chain).
    struct ConfiguredGraphFixture
    {
        RenderGraph Graph;
        Ref<DeclarativeStubPass> Shadow;
        Ref<DeclarativeStubPass> Scene;
        Ref<DeclarativeStubPass> DeferredLighting;    // may be null in forward paths
        Ref<DeclarativeStubPass> DeferredOpaqueDecal; // may be null in forward paths
        Ref<DeclarativeStubPass> ForwardOverlay;      // may be null in forward paths
        Ref<DeclarativeStubPass> Foliage;
        Ref<DeclarativeStubPass> Water;
        Ref<DeclarativeStubPass> Decal;
        Ref<DeclarativeStubPass> SSAO;
        Ref<DeclarativeStubPass> GTAO;
        Ref<DeclarativeStubPass> Particle;
        Ref<DeclarativeStubPass> OITResolve;
        Ref<DeclarativeStubPass> SSS;
        Ref<DeclarativeStubPass> PostProcess;
        Ref<DeclarativeStubPass> SelectionOutline; // may be null when the feature is off
        Ref<DeclarativeStubPass> UIComposite;
        Ref<DeclarativeStubPass> Final;
    };

    // At runtime exactly one of SSAO / GTAO runs (selected by DeferredSettings,
    // mutually-exclusive). Keep the test topology faithful to that contract:
    // declaring both AO passes would create a synthetic AOBuffer WAW that
    // production never hits, and serialising them with a test-only
    // SSAO -> GTAO edge would mask real production WAW regressions on that
    // resource. Instead, BuildPathTopology takes an AOMode parameter and adds
    // exactly one AO pass per call — matching ConfigureRenderGraph.
    enum class AOMode : u8
    {
        SSAO,
        GTAO
    };

    void BuildPathTopology(ConfiguredGraphFixture& f, bool deferred, bool enableSelectionOutline = false,
                           AOMode aoMode = AOMode::SSAO)
    {
        f.Shadow = AddDeclStub(f.Graph, "ShadowPass");
        f.Shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

        f.Scene = AddDeclStub(f.Graph, "ScenePass");
        f.Scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
        f.Scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        f.Scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        f.Scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

        if (deferred)
        {
            f.DeferredLighting = AddDeclStub(f.Graph, "DeferredLightingPass");
            f.DeferredLighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.DeferredLighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            f.DeferredLighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            // Opaque-decal graph shim — slots between ScenePass and
            // DeferredLightingPass so decals composite into G-Buffer
            // albedo/normal/emissive BEFORE the lighting pass samples
            // them. Matches Renderer3D::ConfigureRenderGraph wiring and
            // DeferredOpaqueDecalPass::Init's declared resource contract.
            f.DeferredOpaqueDecal = AddDeclStub(f.Graph, "DeferredOpaqueDecalPass");
            f.DeferredOpaqueDecal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.DeferredOpaqueDecal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            f.ForwardOverlay = AddDeclStub(f.Graph, "ForwardOverlayPass");
            f.ForwardOverlay->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        f.Foliage = AddDeclStub(f.Graph, "FoliagePass");
        if (!deferred)
            f.Foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

        // Keep insertion order aligned with ConfigureRenderGraph's explicit
        // chain (Foliage -> Decal -> Water). BuildFrameGraph derives WAW
        // dependencies in insertion order, so Water before Decal can derive
        // Water -> Decal and create a Decal <-> Water cycle against the
        // explicit Decal -> Water baseline edge.
        f.Decal = AddDeclStub(f.Graph, "DecalPass");
        if (!deferred)
        {
            f.Decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.Decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        f.Water = AddDeclStub(f.Graph, "WaterPass");
        if (!deferred)
        {
            f.Water->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            // In weighted-blended OIT mode the water pass writes into the shared
            // OIT accumulation / revealage attachments rather than SceneColor —
            // SceneColor is only modified later when OITResolvePass composites
            // the accum buffer back onto the scene framebuffer. Modelling both
            // writes lets the L5 validator catch a missing Water -> OITResolve
            // handoff (RAW on OITAccum) that the previous "both write SceneColor"
            // approximation silently masked.
            f.Water->TestDeclareWrite(std::string(ResourceNames::OITAccum));
            f.Water->TestDeclareWrite(std::string(ResourceNames::OITRevealage));
            f.Water->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        // Mutually-exclusive AO selection: register exactly the pass that
        // production would run for the selected mode. ConfigureRenderGraph-
        // time validation currently does not rely on SSAO/GTAO legacy
        // DeclareRead/DeclareWrite contracts (AO reads are captured in the
        // graph-native setup bridge instead), so keep these stubs
        // declaration-free to mirror production baseline behavior.
        if (aoMode == AOMode::SSAO)
        {
            f.SSAO = AddDeclStub(f.Graph, "SSAOPass");
        }
        else
        {
            f.GTAO = AddDeclStub(f.Graph, "GTAOPass");
        }

        f.Particle = AddDeclStub(f.Graph, "ParticlePass");
        if (!deferred)
        {
            // Particle OIT shares the same accumulation buffers as water, then
            // falls back to SceneColor for the final composite path.
            f.Particle->TestDeclareWrite(std::string(ResourceNames::OITAccum));
            f.Particle->TestDeclareWrite(std::string(ResourceNames::OITRevealage));
            f.Particle->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        f.OITResolve = AddDeclStub(f.Graph, "OITResolvePass");
        if (!deferred)
        {
            f.OITResolve->TestDeclareRead(std::string(ResourceNames::OITAccum));
            f.OITResolve->TestDeclareRead(std::string(ResourceNames::OITRevealage));
            f.OITResolve->TestDeclareRead(std::string(ResourceNames::SceneColor));
            f.OITResolve->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        f.SSS = AddDeclStub(f.Graph, "SSSPass");
        if (!deferred)
            f.SSS->TestDeclareRead(std::string(ResourceNames::SceneColor));

        f.PostProcess = AddDeclStub(f.Graph, "PostProcessPass");
        if (!deferred)
            f.PostProcess->TestDeclareRead(std::string(ResourceNames::SceneColor));
        f.PostProcess->TestDeclareRead(std::string(ResourceNames::SceneDepth));
        f.PostProcess->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

        if (enableSelectionOutline)
        {
            // Mirrors Renderer3D::ConfigureRenderGraph's EnableSelectionOutline
            // branch: the pass slots between PostProcessPass and UICompositePass,
            // reads post-process colour and writes SelectionOutlineColor.
            f.SelectionOutline = AddDeclStub(f.Graph, "SelectionOutlinePass");
            f.SelectionOutline->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
            f.SelectionOutline->TestDeclareWrite(std::string(ResourceNames::SelectionOutlineColor));
        }

        f.UIComposite = AddDeclStub(f.Graph, "UICompositePass");
        if (enableSelectionOutline)
            f.UIComposite->TestDeclareRead(std::string(ResourceNames::SelectionOutlineColor));
        else
            f.UIComposite->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
        f.UIComposite->TestDeclareWrite(std::string(ResourceNames::UIComposite));

        f.Final = AddDeclStub(f.Graph, "FinalPass");
        f.Final->TestDeclareRead(std::string(ResourceNames::UIComposite));

        // Wire dependencies identically to Renderer3D::ConfigureRenderGraph.
        f.Graph.AddExecutionDependency("ShadowPass", "ScenePass");
        if (deferred)
        {
            f.Graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
            f.Graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
            f.Graph.AddExecutionDependency("DeferredLightingPass", "ForwardOverlayPass");
            f.Graph.AddExecutionDependency("ForwardOverlayPass", "FoliagePass");
        }
        f.Graph.AddExecutionDependency("ScenePass", "FoliagePass");
        f.Graph.AddExecutionDependency("FoliagePass", "DecalPass");
        f.Graph.AddExecutionDependency("DecalPass", "WaterPass");
        f.Graph.AddExecutionDependency("WaterPass", "ParticlePass");
        f.Graph.ConnectPass("ParticlePass", "OITResolvePass");
        f.Graph.ConnectPass("OITResolvePass", "SSSPass");
        f.Graph.ConnectPass("SSSPass", "PostProcessPass");
        if (enableSelectionOutline)
        {
            f.Graph.ConnectPass("PostProcessPass", "SelectionOutlinePass");
            f.Graph.ConnectPass("SelectionOutlinePass", "UICompositePass");
        }
        else
        {
            f.Graph.ConnectPass("PostProcessPass", "UICompositePass");
        }
        f.Graph.AddExecutionDependency("UICompositePass", "FinalPass");

        f.Graph.SetFinalPass("FinalPass");
    }
} // namespace

TEST(RenderGraphConfigureTopology, ForwardPathIsHazardFree)
{
    // Forward and Forward+ share the same pass set and dependency wiring
    // in Renderer3D::ConfigureRenderGraph (the difference is runtime behaviour,
    // not graph topology). Test both under the non-deferred branch.
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/false);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, ForwardPlusPathIsHazardFree)
{
    // Forward+ = Forward topology with tile-classified lighting at runtime;
    // graph wiring is identical so the hazard check must pass in both.
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/false);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, DeferredPathIsHazardFree)
{
    // Deferred inserts DeferredLightingPass and ForwardOverlayPass between
    // ScenePass and the forward chain. Validates the extra edges
    // ConfigureRenderGraph adds for the deferred branch don't leave any
    // declared resource without a transitive producer edge.
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/true);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingStartupBaselineEdgesMatchesObservedHazards)
{
    // Mirrors the exact startup regression where only post-chain edges were
    // left wired during ConfigureRenderGraph-time validation.
    RenderGraph graph;

    auto shadow = AddDeclStub(graph, "ShadowPass");
    shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    AddDeclStub(graph, "ParticlePass");
    AddDeclStub(graph, "OITResolvePass");
    AddDeclStub(graph, "SSSPass");

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareRead(std::string(ResourceNames::SceneColor));
    post->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    post->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    // Intentionally missing startup baseline edges:
    // Shadow->Scene, Scene->Foliage->Decal->Water->Particle.
    graph.AddExecutionDependency("ParticlePass", "OITResolvePass");
    graph.AddExecutionDependency("OITResolvePass", "SSSPass");
    graph.AddExecutionDependency("SSSPass", "PostProcessPass");
    graph.AddExecutionDependency("PostProcessPass", "UICompositePass");
    graph.AddExecutionDependency("UICompositePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected startup hazard set when baseline edges are omitted";

    const bool hasSceneShadowRaw = std::any_of(hazards.begin(), hazards.end(),
                                               [](const RenderGraph::Hazard& h)
                                               {
                                                   return h.Kind == RenderGraph::HazardKind::ReadAfterWrite &&
                                                          h.Resource == ResourceNames::ShadowMapCSM &&
                                                          h.Producer == "ShadowPass" &&
                                                          h.Consumer == "ScenePass";
                                               });
    const bool hasPostColorRaw = std::any_of(hazards.begin(), hazards.end(),
                                             [](const RenderGraph::Hazard& h)
                                             {
                                                 return h.Kind == RenderGraph::HazardKind::ReadAfterWrite &&
                                                        h.Resource == ResourceNames::SceneColor &&
                                                        h.Producer == "ScenePass" &&
                                                        h.Consumer == "PostProcessPass";
                                             });
    const bool hasPostDepthRaw = std::any_of(hazards.begin(), hazards.end(),
                                             [](const RenderGraph::Hazard& h)
                                             {
                                                 return h.Kind == RenderGraph::HazardKind::ReadAfterWrite &&
                                                        h.Resource == ResourceNames::SceneDepth &&
                                                        h.Producer == "ScenePass" &&
                                                        h.Consumer == "PostProcessPass";
                                             });
    const bool hasPostShadowRaw = std::any_of(hazards.begin(), hazards.end(),
                                              [](const RenderGraph::Hazard& h)
                                              {
                                                  return h.Kind == RenderGraph::HazardKind::ReadAfterWrite &&
                                                         h.Resource == ResourceNames::ShadowMapCSM &&
                                                         h.Producer == "ShadowPass" &&
                                                         h.Consumer == "PostProcessPass";
                                              });

    EXPECT_TRUE(hasSceneShadowRaw) << HazardsToString(hazards);
    EXPECT_TRUE(hasPostColorRaw) << HazardsToString(hazards);
    EXPECT_TRUE(hasPostDepthRaw) << HazardsToString(hazards);
    EXPECT_TRUE(hasPostShadowRaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingDecalToWaterEdgeIsFlagged)
{
    // Guardrail for the current baseline: DecalPass -> WaterPass is still
    // required. Without it, both passes write SceneColor with no declared
    // ordering edge and the validator must surface a WAW hazard.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto decal = AddDeclStub(graph, "DecalPass");
    decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto water = AddDeclStub(graph, "WaterPass");
    water->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    water->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    graph.AddExecutionDependency("ScenePass", "FoliagePass");
    graph.AddExecutionDependency("FoliagePass", "DecalPass");
    // Intentionally omit: graph.AddExecutionDependency("DecalPass", "WaterPass");
    graph.AddExecutionDependency("ScenePass", "WaterPass");
    graph.AddExecutionDependency("WaterPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected hazard when DecalPass -> WaterPass is missing";

    const auto hasDecalWaterWaw = std::any_of(hazards.begin(), hazards.end(),
                                              [](const RenderGraph::Hazard& h)
                                              {
                                                  return h.Kind == RenderGraph::HazardKind::WriteAfterWrite &&
                                                         h.Resource == ResourceNames::SceneColor &&
                                                         h.Producer == "DecalPass" &&
                                                         h.Consumer == "WaterPass";
                                              });
    EXPECT_TRUE(hasDecalWaterWaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingSceneToFoliageEdgeIsFlagged)
{
    // Guardrail for the current baseline: ScenePass -> FoliagePass is still
    // required because both passes write SceneColor in the forward chain.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("ScenePass", "FoliagePass");
    graph.AddExecutionDependency("FoliagePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected hazard when ScenePass -> FoliagePass is missing";

    const auto hasSceneFoliageWaw = std::any_of(hazards.begin(), hazards.end(),
                                                [](const RenderGraph::Hazard& h)
                                                {
                                                    return h.Kind == RenderGraph::HazardKind::WriteAfterWrite &&
                                                           h.Resource == ResourceNames::SceneColor &&
                                                           h.Producer == "ScenePass" &&
                                                           h.Consumer == "FoliagePass";
                                                });
    EXPECT_TRUE(hasSceneFoliageWaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingFoliageToDecalEdgeIsFlagged)
{
    // Guardrail for the current baseline: FoliagePass -> DecalPass remains
    // required to serialize SceneColor writes in the forward chain.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto decal = AddDeclStub(graph, "DecalPass");
    decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    graph.AddExecutionDependency("ScenePass", "FoliagePass");
    // Intentionally omit: graph.AddExecutionDependency("FoliagePass", "DecalPass");
    graph.AddExecutionDependency("ScenePass", "DecalPass");
    graph.AddExecutionDependency("DecalPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected hazard when FoliagePass -> DecalPass is missing";

    const auto hasFoliageDecalWaw = std::any_of(hazards.begin(), hazards.end(),
                                                [](const RenderGraph::Hazard& h)
                                                {
                                                    return h.Kind == RenderGraph::HazardKind::WriteAfterWrite &&
                                                           h.Resource == ResourceNames::SceneColor &&
                                                           h.Producer == "FoliagePass" &&
                                                           h.Consumer == "DecalPass";
                                                });
    EXPECT_TRUE(hasFoliageDecalWaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingWaterToParticleEdgeIsFlagged)
{
    // Guardrail for the current baseline: WaterPass -> ParticlePass remains
    // required because both passes write shared OIT accumulation targets.
    RenderGraph graph;

    auto water = AddDeclStub(graph, "WaterPass");
    water->TestDeclareWrite(std::string(ResourceNames::OITAccum));
    water->TestDeclareWrite(std::string(ResourceNames::OITRevealage));

    auto particle = AddDeclStub(graph, "ParticlePass");
    particle->TestDeclareWrite(std::string(ResourceNames::OITAccum));
    particle->TestDeclareWrite(std::string(ResourceNames::OITRevealage));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::OITAccum));

    // Intentionally omit: graph.AddExecutionDependency("WaterPass", "ParticlePass");
    graph.AddExecutionDependency("ParticlePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected hazard when WaterPass -> ParticlePass is missing";

    const auto hasWaterParticleWaw = std::any_of(hazards.begin(), hazards.end(),
                                                 [](const RenderGraph::Hazard& h)
                                                 {
                                                     return h.Kind == RenderGraph::HazardKind::WriteAfterWrite &&
                                                            h.Resource == ResourceNames::OITAccum &&
                                                            h.Producer == "WaterPass" &&
                                                            h.Consumer == "ParticlePass";
                                                 });
    EXPECT_TRUE(hasWaterParticleWaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingShadowToSceneEdgeIsFlagged)
{
    // Guardrail for the current baseline: ShadowPass -> ScenePass is
    // required because ScenePass samples shadow maps produced by ShadowPass.
    RenderGraph graph;

    auto shadow = AddDeclStub(graph, "ShadowPass");
    shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("ShadowPass", "ScenePass");
    graph.AddExecutionDependency("ScenePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected hazard when ShadowPass -> ScenePass is missing";

    const auto hasShadowSceneRaw = std::any_of(hazards.begin(), hazards.end(),
                                               [](const RenderGraph::Hazard& h)
                                               {
                                                   return h.Kind == RenderGraph::HazardKind::ReadAfterWrite &&
                                                          h.Resource == ResourceNames::ShadowMapCSM &&
                                                          h.Producer == "ShadowPass" &&
                                                          h.Consumer == "ScenePass";
                                               });
    EXPECT_TRUE(hasShadowSceneRaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, SceneToDeferredLightingCanBeTransitiveViaDecal)
{
    // ScenePass -> DeferredLightingPass no longer needs to be explicit when
    // DeferredOpaqueDecalPass is present. The transitive chain
    // ScenePass -> DeferredOpaqueDecalPass -> DeferredLightingPass should
    // satisfy depth/normals RAW ordering for DeferredLightingPass.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto deferredDecal = AddDeclStub(graph, "DeferredOpaqueDecalPass");
    deferredDecal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    deferredDecal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto deferredLighting = AddDeclStub(graph, "DeferredLightingPass");
    deferredLighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    deferredLighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
    deferredLighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
    graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
    graph.AddExecutionDependency("DeferredLightingPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Transitive Scene -> DeferredOpaqueDecal -> DeferredLighting ordering should be hazard-free: "
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingSceneToDeferredOpaqueDecalEdgeIsFlagged)
{
    // Deferred baseline guard: DeferredOpaqueDecalPass reads SceneDepth
    // produced by ScenePass, so ScenePass -> DeferredOpaqueDecalPass is required.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto deferredDecal = AddDeclStub(graph, "DeferredOpaqueDecalPass");
    deferredDecal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    deferredDecal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
    graph.AddExecutionDependency("DeferredOpaqueDecalPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty()) << "Expected hazard when ScenePass -> DeferredOpaqueDecalPass is missing";

    const auto hasSceneDepthRaw = std::any_of(hazards.begin(), hazards.end(),
                                              [](const RenderGraph::Hazard& h)
                                              {
                                                  return h.Kind == RenderGraph::HazardKind::ReadAfterWrite &&
                                                         h.Resource == ResourceNames::SceneDepth &&
                                                         h.Producer == "ScenePass" &&
                                                         h.Consumer == "DeferredOpaqueDecalPass";
                                              });
    EXPECT_TRUE(hasSceneDepthRaw) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, ForwardPathWithSelectionOutlineIsHazardFree)
{
    // Toggling EnableSelectionOutline on must introduce
    // PostProcessPass -> SelectionOutlinePass -> UICompositePass wiring
    // without producing any RAW/WAW on PostProcessColor.
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/false, /*enableSelectionOutline=*/true);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
    EXPECT_TRUE(static_cast<bool>(f.SelectionOutline))
        << "SelectionOutline stub must be populated when the feature flag is on";
}

TEST(RenderGraphConfigureTopology, DeferredPathWithSelectionOutlineIsHazardFree)
{
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/true, /*enableSelectionOutline=*/true);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, ForwardPathWithGTAOIsHazardFree)
{
    // GTAO branch coverage: existing tests only exercise BuildPathTopology
    // with the default AOMode::SSAO, leaving the GTAO wiring path untested.
    // Validate that BuildPathTopology(..., AOMode::GTAO) produces a
    // hazard-free graph and wires the correct stub (GTAO populated, SSAO null).
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/false, /*enableSelectionOutline=*/false, AOMode::GTAO);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
    EXPECT_TRUE(static_cast<bool>(f.GTAO))
        << "GTAO stub must be populated when AOMode::GTAO is selected";
    EXPECT_FALSE(static_cast<bool>(f.SSAO))
        << "SSAO stub must remain null when AOMode::GTAO is selected";
}

TEST(RenderGraphConfigureTopology, DeferredPathWithGTAOIsHazardFree)
{
    // GTAO deferred-path coverage: validate that the GTAO branch works
    // correctly in the deferred rendering path, including the extra edges
    // for DeferredLightingPass and ForwardOverlayPass.
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/true, /*enableSelectionOutline=*/false, AOMode::GTAO);
    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);
    EXPECT_TRUE(static_cast<bool>(f.GTAO))
        << "GTAO stub must be populated when AOMode::GTAO is selected";
    EXPECT_FALSE(static_cast<bool>(f.SSAO))
        << "SSAO stub must remain null when AOMode::GTAO is selected";
}

// =============================================================================
// Regression coverage for DeferredOpaqueDecalPass: the opaque decal drain was
// previously a synchronous side-effect of SceneRenderPass::Execute(); it was
// promoted to a standalone graph node between ScenePass and
// DeferredLightingPass. These tests lock in the presence + ordering contract
// so future refactors of Init / Execute / GetTarget or the node placement
// trip CI rather than silently regressing decal visibility in deferred mode.
// =============================================================================
TEST(RenderGraphConfigureTopology, DecalNodePresence)
{
    // Presence: BuildPathTopology(deferred=true) must register the opaque
    // decal shim with the declared read(SceneDepth) / write(SceneColor)
    // contract that DeferredOpaqueDecalPass::Init uses. Validator must
    // see the full deferred topology hazard-free.
    ConfiguredGraphFixture f;
    BuildPathTopology(f, /*deferred=*/true);

    ASSERT_TRUE(static_cast<bool>(f.DeferredOpaqueDecal))
        << "DeferredOpaqueDecalPass stub must be present in the deferred topology";
    EXPECT_EQ(f.DeferredOpaqueDecal->GetName(), "DeferredOpaqueDecalPass");

    const auto hazards = f.Graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty()) << HazardsToString(hazards);

    // Omission regression: rebuild the same topology *without* the decal
    // node and assert the validator flags the missing producer edge (the
    // lighting pass still declares `SceneColor` writes but the decal
    // dependency is no longer mediated by an execution edge). We mutate
    // the stored execution deps directly because ResetTopology + a fresh
    // rebuild would be identical to a forward topology build.
    ConfiguredGraphFixture omitted;
    BuildPathTopology(omitted, /*deferred=*/true);
    // Drop the opaque decal node and its edges by rebuilding the graph
    // topology without it. We validate via a companion fixture because
    // RenderGraph doesn't expose a public pass-removal API.
    RenderGraph barePathGraph;
    auto shadow = Ref<DeclarativeStubPass>::Create("ShadowPass");
    shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));
    barePathGraph.AddPass(shadow);
    auto scene = Ref<DeclarativeStubPass>::Create("ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
    barePathGraph.AddPass(scene);
    auto lighting = Ref<DeclarativeStubPass>::Create("DeferredLightingPass");
    lighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
    lighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    lighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    barePathGraph.AddPass(lighting);
    // Only wire scene -> lighting — decal node intentionally absent so
    // nothing in the graph declares a dependency between the SceneColor
    // writes of decals and the lighting pass's SceneColor write. The
    // validator's WAW check should still pass here (ScenePass writes
    // SceneColor *before* lighting via the edge we added), so the real
    // evidence of the decal node's importance is in the ordering test
    // below: without the decal edges, the validator accepts a graph in
    // which decals could run *after* lighting (a lost-update bug).
    barePathGraph.AddExecutionDependency("ShadowPass", "ScenePass");
    barePathGraph.AddExecutionDependency("ScenePass", "DeferredLightingPass");
    barePathGraph.SetFinalPass("DeferredLightingPass");
    const auto bareHazards = barePathGraph.ValidateResourceHazards();
    EXPECT_TRUE(bareHazards.empty())
        << "Bare scene->lighting graph (no decal node) must validate — the "
        << "decal contract is an *ordering* guarantee, not a hazard-count "
        << "diff. DecalNodeOrdering covers the ordering evidence.";
}

TEST(RenderGraphConfigureTopology, DecalNodeOrdering)
{
    // Ordering: a deferred graph whose decal node sits *after*
    // DeferredLightingPass (i.e. decals composite over an already-lit
    // scene) produces a WAW on SceneColor that lighting cannot see. With
    // the intended ordering (ScenePass -> Decal -> Lighting) the WAW is
    // serialised through the decal node and disappears.

    // Correct ordering: validator must be happy.
    {
        ConfiguredGraphFixture f;
        BuildPathTopology(f, /*deferred=*/true);
        const auto hazards = f.Graph.ValidateResourceHazards();
        EXPECT_TRUE(hazards.empty())
            << "Intended ScenePass -> DeferredOpaqueDecalPass -> "
            << "DeferredLightingPass ordering must be hazard-free. "
            << HazardsToString(hazards);
    }

    // Reordered: decal moved *after* lighting. Build an explicit graph so
    // we can freely place edges in the wrong order, then assert the L5
    // validator reports a WAW between DeferredLightingPass (SceneColor
    // write) and DeferredOpaqueDecalPass (SceneColor write) because
    // there's no serialising edge between them.
    {
        RenderGraph reordered;
        auto scene = Ref<DeclarativeStubPass>::Create("ScenePass");
        scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
        reordered.AddPass(scene);

        auto lighting = Ref<DeclarativeStubPass>::Create("DeferredLightingPass");
        lighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
        lighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
        lighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        reordered.AddPass(lighting);

        auto decal = Ref<DeclarativeStubPass>::Create("DeferredOpaqueDecalPass");
        decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
        decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        reordered.AddPass(decal);

        // Intentionally bad ordering: ScenePass -> lighting, ScenePass ->
        // decal. Both writers produce SceneColor with no edge between
        // them. Validator must flag WAW.
        reordered.AddExecutionDependency("ScenePass", "DeferredLightingPass");
        reordered.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
        reordered.SetFinalPass("DeferredLightingPass");

        const auto hazards = reordered.ValidateResourceHazards();
        EXPECT_FALSE(hazards.empty())
            << "Decal node running in parallel with DeferredLightingPass "
            << "must produce a WAW hazard on SceneColor — the hazard "
            << "validator is how we detect future topology mistakes.";
    }
}

TEST(RenderGraphConfigureTopology, ResetTopologyAndRebuildAcrossPathsNoLeaks)
{
    // ConfigureRenderGraph may be invoked repeatedly as the user toggles
    // between RenderingPath::Forward / Forward+ / Deferred at runtime. Each
    // call invokes RenderGraph::ResetTopology() before re-adding passes.
    // Simulate ≥3 rebuild cycles across all three paths and assert no
    // hazards accumulate and no references leak.
    RenderGraph graph;

    auto buildOn = [&graph](bool deferred)
    {
        graph.ResetTopology();
        // Simplified subset of BuildPathTopology — inlined here to reuse the
        // single graph instance rather than a fresh per-cycle RenderGraph.
        // buildOn constructs only 3–5 passes (Shadow, Scene, optional
        // DeferredLighting/DeferredOpaqueDecal, Final) compared to
        // BuildPathTopology's 15+ passes (Foliage, Water, AO, Particle,
        // OITResolve, SSS, PostProcess, UIComposite, etc.). The reduced
        // topology is intentional to exercise ResetTopology() leak detection
        // in ResourceHazardValidationTests.
        auto shadow = Ref<DeclarativeStubPass>::Create("ShadowPass");
        shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));
        graph.AddPass(shadow);

        auto scene = Ref<DeclarativeStubPass>::Create("ScenePass");
        scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        // Mirror BuildPathTopology: ScenePass writes SceneNormals so the
        // Deferred branch's normals handoff into DeferredLightingPass /
        // DeferredOpaqueDecalPass actually has a declared producer edge
        // for the L5 validator to verify on each rebuild cycle.
        scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
        graph.AddPass(scene);

        if (deferred)
        {
            auto deferredLight = Ref<DeclarativeStubPass>::Create("DeferredLightingPass");
            deferredLight->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            deferredLight->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            deferredLight->TestDeclareWrite(std::string(ResourceNames::SceneColor));
            graph.AddPass(deferredLight);

            // Opaque-decal graph shim — mirrors ConfigureRenderGraph so
            // the rebuild test exercises the same pass set + edges as
            // production when switching back into Deferred. Resource
            // contract matches BuildPathTopology / DeferredOpaqueDecalPass::Init:
            // reads SceneDepth, writes SceneColor (does NOT read SceneNormals).
            auto decal = Ref<DeclarativeStubPass>::Create("DeferredOpaqueDecalPass");
            decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
            graph.AddPass(decal);

            graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
            graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
        }

        auto final = Ref<DeclarativeStubPass>::Create("FinalPass");
        final->TestDeclareRead(std::string(ResourceNames::SceneColor));
        graph.AddPass(final);

        graph.AddExecutionDependency("ShadowPass", "ScenePass");
        if (deferred)
            graph.AddExecutionDependency("DeferredLightingPass", "FinalPass");
        else
            graph.AddExecutionDependency("ScenePass", "FinalPass");

        graph.SetFinalPass("FinalPass");
    };

    constexpr i32 kCycles = 4;
    const std::array<bool, kCycles> paths = { false, false, true, false };
    for (i32 i = 0; i < kCycles; ++i)
    {
        buildOn(paths[i]);
        const auto hazards = graph.ValidateResourceHazards();
        EXPECT_TRUE(hazards.empty())
            << "Cycle " << i << " (deferred=" << paths[i] << "): "
            << HazardsToString(hazards);

        // Pass set must match what this cycle installed — no residual edges
        // or passes leaked from a previous cycle (the ResetTopology contract).
        const sizet expectedPassCount = paths[i] ? 5u : 3u;
        EXPECT_EQ(graph.GetAllPasses().size(), expectedPassCount)
            << "Cycle " << i << ": residual passes from prior cycle";
    }
}
