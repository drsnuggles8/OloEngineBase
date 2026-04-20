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
        void TestDeclareRead(std::string_view name)
        {
            DeclareRead(name);
        }
        void TestDeclareWrite(std::string_view name)
        {
            DeclareWrite(name);
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
    post->TestDeclareWrite("FinalColor");

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
    final->TestDeclareWrite(std::string(ResourceNames::FinalColor));

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
    final->TestDeclareWrite(std::string(ResourceNames::FinalColor));

    graph.ConnectPass("PostProcessPass", "UICompositePass");
    graph.ConnectPass("UICompositePass", "FinalPass");

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
