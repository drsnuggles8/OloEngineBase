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
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/PassGraphNode.h"
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

    ResourceHandle::Kind NormalizeDeclarationKind(const ResourceHandle::Kind kind)
    {
        return kind == ResourceHandle::Kind::Unknown ? ResourceHandle::Kind::Texture2D : kind;
    }

    void MirrorPassRead(RGBuilder& builder, const ResourceHandle& resource)
    {
        const auto kind = NormalizeDeclarationKind(resource.Type);
        const auto desc = RGResourceDesc::FromLegacy(kind, resource.Name);

        switch (kind)
        {
            case ResourceHandle::Kind::Framebuffer:
            {
                auto handle = builder.ImportFramebuffer(resource.Name, nullptr, desc);
                [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::RenderTargetRead);
                break;
            }
            case ResourceHandle::Kind::UniformBuffer:
            case ResourceHandle::Kind::StorageBuffer:
            {
                auto handle = builder.ImportBuffer(resource.Name, 0, desc);
                [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderStorage);
                break;
            }
            default:
            {
                auto handle = builder.ImportTexture(resource.Name, 0, desc);
                [[maybe_unused]] const auto readHandle = builder.Read(handle, RGReadUsage::ShaderSample);
                break;
            }
        }
    }

    void MirrorPassWrite(RGBuilder& builder, const ResourceHandle& resource)
    {
        const auto kind = NormalizeDeclarationKind(resource.Type);
        const auto desc = RGResourceDesc::FromLegacy(kind, resource.Name);

        switch (kind)
        {
            case ResourceHandle::Kind::Framebuffer:
            {
                auto handle = builder.ImportFramebuffer(resource.Name, nullptr, desc);
                builder.Write(handle, RGWriteUsage::RenderTarget);
                break;
            }
            case ResourceHandle::Kind::UniformBuffer:
            case ResourceHandle::Kind::StorageBuffer:
            {
                auto handle = builder.ImportBuffer(resource.Name, 0, desc);
                builder.Write(handle, RGWriteUsage::ShaderStorage);
                break;
            }
            default:
            {
                auto handle = builder.ImportTexture(resource.Name, 0, desc);
                builder.Write(handle, RGWriteUsage::RenderTarget);
                break;
            }
        }
    }

    void MirrorPassDeclarations(RGBuilder& builder, const RenderPass& pass)
    {
        for (const auto& read : pass.GetReads())
            MirrorPassRead(builder, read);

        for (const auto& write : pass.GetWrites())
            MirrorPassWrite(builder, write);
    }

    void RegisterDeclarativeStubNode(RenderGraph& graph, const Ref<DeclarativeStubPass>& pass)
    {
        auto node = Ref<PassGraphNode>::Create(
            std::string(pass->GetName()),
            pass.As<RenderPass>(),
            [pass](RGBuilder& builder, FrameBlackboard& /*blackboard*/)
            {
                MirrorPassDeclarations(builder, *pass);
            });
        graph.AddNode(node);
    }

    Ref<DeclarativeStubPass> AddDeclStub(RenderGraph& graph, const std::string& name)
    {
        auto pass = Ref<DeclarativeStubPass>::Create(name);
        pass->SetName(name);
        RegisterDeclarativeStubNode(graph, pass);
        return pass;
    }

    class SetupOnlyGraphNode : public RenderGraphNode
    {
      public:
        using SetupFn = std::function<void(RGBuilder&)>;

        explicit SetupOnlyGraphNode(std::string name, SetupFn setup)
            : m_Name(std::move(name)), m_Setup(std::move(setup))
        {
        }

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

        [[nodiscard]] RenderGraphNodeFlags GetFlags() const override
        {
            return RenderGraphNodeFlags::Graphics;
        }

      private:
        std::string m_Name;
        SetupFn m_Setup;
    };

    Ref<SetupOnlyGraphNode> AddSetupNode(RenderGraph& graph, std::string name, SetupOnlyGraphNode::SetupFn setup)
    {
        auto node = Ref<SetupOnlyGraphNode>::Create(std::move(name), std::move(setup));
        graph.AddNode(node);
        return node;
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
// Phase F slice 27 — declaration-derived edge synthesis.
// A DeclareWrite + DeclareRead pair on the same resource name is sufficient
// for ValidateResourceHazards to infer the RAW ordering edge automatically.
// No explicit AddExecutionDependency / ConnectPass call is required.
// This is the core invariant introduced in slice 27.
// =============================================================================
TEST(RenderGraphResourceHazards, DeclaredRAWPair_DerivedEdgePreventsHazardWithoutExplicitDependency)
{
    RenderGraph graph;
    auto shadow = AddDeclStub(graph, "Shadow");
    auto scene = AddDeclStub(graph, "Scene");

    shadow->TestDeclareWrite("ShadowMap");
    scene->TestDeclareRead("ShadowMap");
    // NOTE: no ConnectPass / AddExecutionDependency — slice 27 derives the
    // RAW edge from the declaration pair alone.

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: declared RAW pair should not require an explicit edge."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 27 — declaration-chain transitivity.
// A writes X, B reads X (derived: B depends on A).
// B writes Y, C reads Y (derived: C depends on B, and transitively A).
// No explicit edges at all — all RAW ordering is derived from declarations.
// =============================================================================
TEST(RenderGraphResourceHazards, Slice27_DeclarationChainTransitivityIsHazardFree)
{
    RenderGraph graph;
    auto a = AddDeclStub(graph, "PassA");
    auto b = AddDeclStub(graph, "PassB");
    auto c = AddDeclStub(graph, "PassC");

    a->TestDeclareWrite("ResourceX");
    b->TestDeclareRead("ResourceX");
    b->TestDeclareWrite("ResourceY");
    c->TestDeclareRead("ResourceY");
    // No explicit edges — slice 27 derives A→B from X and B→C from Y,
    // then transitivity propagates A into C's closure.

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: two-hop declaration chain must be fully derived "
           "without any explicit AddExecutionDependency calls."
        << HazardsToString(hazards);
}

// =============================================================================
// WAW: two independent passes writing the same resource are a race.
// WAW is NOT derived from declarations (ordering is ambiguous — the graph
// cannot know which writer should run first), so explicit edges are still
// required to serialize concurrent writes.
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
// Phase F slice 27 — production-shaped with ShadowMapCSM read by PostProcess
// but no direct explicit edge ShadowPass → PostProcessPass.
// Slice 27 derives the RAW edge from declarations, so no hazard is expected.
// =============================================================================
TEST(RenderGraphResourceHazards, ProductionShapedGraph_DerivedEdgePreventsHazardForShadow)
{
    RenderGraph graph;
    AddDeclStub(graph, "ShadowPass")->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));
    AddDeclStub(graph, "ScenePass");

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PostProcessColor));

    graph.ConnectPass("ShadowPass", "ScenePass");
    // NOTE: PostProcess has no explicit path to ShadowPass.
    // Slice 27 derives ShadowPass → PostProcessPass from the declaration pair.
    graph.ConnectPass("PostProcessPass", "FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: ShadowPass.DeclareWrite + PostProcessPass.DeclareRead "
           "should derive the ordering edge automatically."
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

// Phase F slice 27: EnvironmentPass.DeclareWrite(PrefilterMap) +
// ScenePass.DeclareRead(PrefilterMap) is sufficient — no explicit edge needed.
TEST(RenderGraphResourceHazards, IblDeclarationsAloneSufficient)
{
    RenderGraph graph;

    auto env = AddDeclStub(graph, "EnvironmentPass");
    env->TestDeclareWrite(std::string(ResourceNames::PrefilterMap));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::PrefilterMap));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    // NOTE: no ConnectPass — slice 27 derives the RAW edge from declarations.
    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: IBL declaration pair alone must be sufficient."
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
    // NOTE: FinalPass has no explicit dependency on UICompositePass.
    // Slice 27 derives the RAW edge from UICompositePass.DeclareWrite(UIComposite)
    // + FinalPass.DeclareRead(UIComposite).  This is the exact edge that was
    // removed from Renderer3D.cpp ConfigureRenderGraph in slice 27.
    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: UIComposite declaration pair must derive the ordering edge."
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

    AddSetupNode(
        graph,
        "FeedbackPass",
        [](RGBuilder& builder)
        {
            auto color = builder.ImportTexture(
                "FeedbackTex",
                21,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "FeedbackTex"));
            [[maybe_unused]] const auto sampled = builder.Read(color, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(0));
            builder.Write(color, RGWriteUsage::RenderTarget, RGSubresourceRange::Mip(0));
        });

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

    AddSetupNode(
        graph,
        "Writer",
        [](RGBuilder& builder)
        {
            auto imported = builder.ImportTexture(
                "ImportedNoBacking",
                0,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ImportedNoBacking"));
            builder.Write(imported, RGWriteUsage::RenderTarget);
        });

    AddSetupNode(
        graph,
        "Reader",
        [](RGBuilder& builder)
        {
            auto imported = builder.ImportTexture(
                "ImportedNoBacking",
                0,
                RGResourceDesc::FromLegacy(ResourceHandle::Kind::Texture2D, "ImportedNoBacking"));
            [[maybe_unused]] const auto sampled = builder.Read(imported, RGReadUsage::ShaderSample);
        });

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
        Ref<DeclarativeStubPass> AOApply; // slice 28 — between SSSPass and PostProcessPass
        Ref<DeclarativeStubPass> PostProcess;
        // Post-process linear chain (slice 28 — all edges derived from declarations)
        Ref<DeclarativeStubPass> Bloom;
        Ref<DeclarativeStubPass> DOF;
        Ref<DeclarativeStubPass> MotionBlur;
        Ref<DeclarativeStubPass> TAA;
        Ref<DeclarativeStubPass> Precipitation;
        Ref<DeclarativeStubPass> Fog;
        Ref<DeclarativeStubPass> ChromAb;
        Ref<DeclarativeStubPass> ColorGrading;
        Ref<DeclarativeStubPass> ToneMap;
        Ref<DeclarativeStubPass> Vignette;
        Ref<DeclarativeStubPass> FXAA;             // may be null when feature is off
        Ref<DeclarativeStubPass> SelectionOutline; // may be null when feature is off
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
                           AOMode aoMode = AOMode::SSAO, bool enableFXAA = false)
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
            // Opaque-decal graph shim — slots between ScenePass and
            // DeferredLightingPass so decals composite into G-Buffer
            // albedo/normal/emissive BEFORE the lighting pass samples
            // them. Matches Renderer3D::ConfigureRenderGraph wiring and
            // DeferredOpaqueDecalPass::Init's declared resource contract.
            // NOTE: registered BEFORE DeferredLightingPass to match production
            // AddPass order (OpaqueDecalPass, DeferredLightPass, ForwardOverlayPass).
            f.DeferredOpaqueDecal = AddDeclStub(f.Graph, "DeferredOpaqueDecalPass");
            f.DeferredOpaqueDecal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.DeferredOpaqueDecal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            f.DeferredLighting = AddDeclStub(f.Graph, "DeferredLightingPass");
            f.DeferredLighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.DeferredLighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            // Phase F slice 33: SceneColor read derives DeferredOpaqueDecalPass→DeferredLightingPass RAW.
            f.DeferredLighting->TestDeclareRead(std::string(ResourceNames::SceneColor));
            f.DeferredLighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

            f.ForwardOverlay = AddDeclStub(f.Graph, "ForwardOverlayPass");
            // Phase F slice 33: SceneColor RMW derives DeferredLightingPass→ForwardOverlayPass RAW.
            f.ForwardOverlay->TestDeclareRead(std::string(ResourceNames::SceneColor));
            f.ForwardOverlay->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        f.Foliage = AddDeclStub(f.Graph, "FoliagePass");
        {
            // Phase F slice 32/33: RMW on SceneColor derives the ordering edge
            // from the preceding pass (ScenePass in forward, ForwardOverlayPass
            // in deferred). Declarations are unconditional in production Init().
            f.Foliage->TestDeclareRead(std::string(ResourceNames::SceneColor));
            f.Foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        // Keep insertion order aligned with ConfigureRenderGraph's declaration-
        // derived chain (Foliage -> Decal -> Water). Each pass declares a
        // SceneColor read-modify-write so RAW edges chain them in order.
        f.Decal = AddDeclStub(f.Graph, "DecalPass");
        {
            // Phase F slice 32/33: SceneColor RMW derives FoliagePass→DecalPass.
            // Declarations are unconditional in production Init().
            f.Decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.Decal->TestDeclareRead(std::string(ResourceNames::SceneColor));
            f.Decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        }

        f.Water = AddDeclStub(f.Graph, "WaterPass");
        {
            // Phase F slice 32/33: SceneColor RMW derives DecalPass→WaterPass.
            // Declarations are unconditional in production Init().
            f.Water->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.Water->TestDeclareRead(std::string(ResourceNames::SceneColor));
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
        // production would run for the selected mode. Slice 30 adds explicit
        // AOBuffer writer declarations so AOApply ordering derives from RAW.
        if (aoMode == AOMode::SSAO)
        {
            f.SSAO = AddDeclStub(f.Graph, "SSAOPass");
            f.SSAO->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.SSAO->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            f.SSAO->TestDeclareWrite(std::string(ResourceNames::AOBuffer));
        }
        else
        {
            f.GTAO = AddDeclStub(f.Graph, "GTAOPass");
            f.GTAO->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.GTAO->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            f.GTAO->TestDeclareWrite(std::string(ResourceNames::AOBuffer));
        }

        f.Particle = AddDeclStub(f.Graph, "ParticlePass");
        {
            // Particle OIT shares the same accumulation buffers as water, then
            // falls back to SceneColor for the final composite path.
            // Phase F slice 32/33: SceneColor read derives WaterPass→ParticlePass RAW.
            // Declarations are unconditional in production Init().
            f.Particle->TestDeclareRead(std::string(ResourceNames::SceneColor));
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
        if (!deferred)
            f.SSS->TestDeclareWrite(std::string(ResourceNames::SSSColor));

        f.AOApply = AddDeclStub(f.Graph, "AOApplyPass");
        if (!deferred)
        {
            f.AOApply->TestDeclareRead(std::string(ResourceNames::SceneColor));
            f.AOApply->TestDeclareRead(std::string(ResourceNames::SSSColor));
            f.AOApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
            f.AOApply->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            f.AOApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));
        }

        // Slice 28: PostProcessPass reads AOApplyColor (not raw SceneColor).
        f.PostProcess = AddDeclStub(f.Graph, "PostProcessPass");
        if (!deferred)
            f.PostProcess->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
        f.PostProcess->TestDeclareRead(std::string(ResourceNames::SceneDepth));
        f.PostProcess->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

        // Post-process linear chain (slice 28).
        // All ordering edges are derived from the matching DeclareWrite/DeclareRead pairs.
        f.Bloom = AddDeclStub(f.Graph, "BloomPass");
        f.Bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
        f.Bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

        f.DOF = AddDeclStub(f.Graph, "DOFPass");
        f.DOF->TestDeclareRead(std::string(ResourceNames::BloomColor));
        f.DOF->TestDeclareWrite(std::string(ResourceNames::DOFColor));

        f.MotionBlur = AddDeclStub(f.Graph, "MotionBlurPass");
        f.MotionBlur->TestDeclareRead(std::string(ResourceNames::DOFColor));
        f.MotionBlur->TestDeclareWrite(std::string(ResourceNames::MotionBlurColor));

        f.TAA = AddDeclStub(f.Graph, "TAAPass");
        f.TAA->TestDeclareRead(std::string(ResourceNames::MotionBlurColor));
        f.TAA->TestDeclareWrite(std::string(ResourceNames::TAAColor));

        f.Precipitation = AddDeclStub(f.Graph, "PrecipitationPass");
        f.Precipitation->TestDeclareRead(std::string(ResourceNames::TAAColor));
        f.Precipitation->TestDeclareWrite(std::string(ResourceNames::PrecipitationColor));

        f.Fog = AddDeclStub(f.Graph, "FogPass");
        f.Fog->TestDeclareRead(std::string(ResourceNames::PrecipitationColor));
        f.Fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

        f.ChromAb = AddDeclStub(f.Graph, "ChromAberrationPass");
        f.ChromAb->TestDeclareRead(std::string(ResourceNames::FogColor));
        f.ChromAb->TestDeclareWrite(std::string(ResourceNames::ChromAbColor));

        f.ColorGrading = AddDeclStub(f.Graph, "ColorGradingPass");
        f.ColorGrading->TestDeclareRead(std::string(ResourceNames::ChromAbColor));
        f.ColorGrading->TestDeclareWrite(std::string(ResourceNames::ColorGradingColor));

        f.ToneMap = AddDeclStub(f.Graph, "ToneMapPass");
        f.ToneMap->TestDeclareRead(std::string(ResourceNames::ColorGradingColor));
        f.ToneMap->TestDeclareWrite(std::string(ResourceNames::ToneMapColor));

        f.Vignette = AddDeclStub(f.Graph, "VignettePass");
        f.Vignette->TestDeclareRead(std::string(ResourceNames::ToneMapColor));
        f.Vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

        if (enableFXAA)
        {
            f.FXAA = AddDeclStub(f.Graph, "FXAAPass");
            f.FXAA->TestDeclareRead(std::string(ResourceNames::VignetteColor));
            f.FXAA->TestDeclareWrite(std::string(ResourceNames::FXAAColor));
        }

        if (enableSelectionOutline)
        {
            // Mirrors Renderer3D::ConfigureRenderGraph's EnableSelectionOutline
            // branch. Declares both possible input resources so the validator
            // derives the correct ordering edge for whichever producer is present.
            f.SelectionOutline = AddDeclStub(f.Graph, "SelectionOutlinePass");
            f.SelectionOutline->TestDeclareRead(std::string(ResourceNames::VignetteColor));
            if (enableFXAA)
                f.SelectionOutline->TestDeclareRead(std::string(ResourceNames::FXAAColor));
            f.SelectionOutline->TestDeclareWrite(std::string(ResourceNames::SelectionOutlineColor));
        }

        f.UIComposite = AddDeclStub(f.Graph, "UICompositePass");
        f.UIComposite->TestDeclareRead(std::string(ResourceNames::VignetteColor));
        if (enableFXAA)
            f.UIComposite->TestDeclareRead(std::string(ResourceNames::FXAAColor));
        if (enableSelectionOutline)
            f.UIComposite->TestDeclareRead(std::string(ResourceNames::SelectionOutlineColor));
        f.UIComposite->TestDeclareWrite(std::string(ResourceNames::UIComposite));

        f.Final = AddDeclStub(f.Graph, "FinalPass");
        f.Final->TestDeclareRead(std::string(ResourceNames::UIComposite));

        // Wire dependencies identically to Renderer3D::ConfigureRenderGraph.
        f.Graph.AddExecutionDependency("ShadowPass", "ScenePass");
        // Phase F slice 33: all deferred-path edges are now derived from
        // declaration pairs — no explicit AddExecutionDependency needed.
        // Phase F slice 32: ScenePass→FoliagePass, FoliagePass→DecalPass,
        // DecalPass→WaterPass, and WaterPass→ParticlePass are all derived from
        // SceneColor RMW declarations — no explicit AddExecutionDependency needed.
        // Phase F slice 31: no explicit Water->AO edge. ScenePass->AO derives
        // from SceneDepth DeclareWrite/DeclareRead pairs.
        // Phase F slice 30: AO mode pass -> AOApply is derived from AOBuffer
        // DeclareWrite/DeclareRead pairs; no explicit AO->Particle edge needed.
        // Phase F slice 29: Particle→OITResolve, OITResolve→SSS, SSS→AOApply are all
        // derived from DeclareRead/DeclareWrite declaration pairs — no explicit edges needed.
        // Phase F slice 28: AOApplyPass→PostProcessPass and all subsequent post-chain
        // edges are derived from DeclareRead/DeclareWrite declaration pairs —
        // no explicit AddExecutionDependency calls needed from AOApplyPass onwards.

        // Phase F slice 27: ShadowPass → ScenePass and UICompositePass →
        // FinalPass are derived from DeclareRead/DeclareWrite declarations;
        // no explicit AddExecutionDependency call is needed here, matching
        // what Renderer3D::ConfigureRenderGraph does post-slice-27.

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

// =============================================================================
// Phase F slice 27 — startup baseline regression.
// Previously ConfigureRenderGraph had explicit edges Shadow→Scene,
// UIComposite→Final and several RAW hazards were caught when those were
// removed.  Slice 27 derives all RAW edges from DeclareRead/DeclareWrite
// declarations, so the same topology that lacked the startup edges is now
// fully hazard-free.  This test documents that the startup regression is
// permanently resolved.
// =============================================================================
TEST(RenderGraphConfigureTopology, StartupBaselineEdges_DerivedEdgesMakeGraphHazardFree)
{
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

    // Simplified topology: AOApplyPass reads SceneColor, PostProcessPass reads AOApplyColor.
    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneColor));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    post->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    post->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto vignette = AddDeclStub(graph, "VignettePass");
    vignette->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    // Only geometry-chain edges wired — NO Shadow→Scene baseline, NO AOApply→Post,
    // NO Vignette→UIComposite, NO UIComposite→Final.
    // Slice 27+28 derives all RAW edges from declarations.
    graph.AddExecutionDependency("ParticlePass", "OITResolvePass");
    graph.AddExecutionDependency("OITResolvePass", "SSSPass");
    graph.AddExecutionDependency("SSSPass", "AOApplyPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27+28: declaration-derived edges must resolve all RAW hazards "
           "even without explicit startup baseline edges."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 28 — post-chain derived edges.
// ValidateResourceHazards synthesises ordering edges for the full post-process
// linear chain from matching DeclareWrite/DeclareRead declaration pairs.
// No explicit AddExecutionDependency calls should be needed in the chain
// PostProcess → Bloom → DOF → MotionBlur → TAA → Precipitation → Fog →
// ChromAb → ColorGrading → ToneMap → Vignette → UIComposite → Final.
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice28_AOApplyPassToPostProcessPassDerivedEdge)
{
    // Minimal: a single RAW pair AOApplyColor write/read derives the edge
    // without any explicit AddExecutionDependency call.
    RenderGraph graph;

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 28: AOApplyColor DeclareWrite/DeclareRead must derive the "
           "AOApplyPass → PostProcessPass ordering edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice28_FullPostChainNoExplicitEdgesIsHazardFree)
{
    // Full post-chain from PostProcess through Vignette → UIComposite → Final
    // with NO explicit edges after PostProcessPass.  Slice 28 derives all
    // ordering edges from matching DeclareWrite/DeclareRead declaration pairs.
    RenderGraph graph;

    auto post = AddDeclStub(graph, "PostProcessPass");
    post->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto dof = AddDeclStub(graph, "DOFPass");
    dof->TestDeclareRead(std::string(ResourceNames::BloomColor));
    dof->TestDeclareWrite(std::string(ResourceNames::DOFColor));

    auto mblur = AddDeclStub(graph, "MotionBlurPass");
    mblur->TestDeclareRead(std::string(ResourceNames::DOFColor));
    mblur->TestDeclareWrite(std::string(ResourceNames::MotionBlurColor));

    auto taa = AddDeclStub(graph, "TAAPass");
    taa->TestDeclareRead(std::string(ResourceNames::MotionBlurColor));
    taa->TestDeclareWrite(std::string(ResourceNames::TAAColor));

    auto precip = AddDeclStub(graph, "PrecipitationPass");
    precip->TestDeclareRead(std::string(ResourceNames::TAAColor));
    precip->TestDeclareWrite(std::string(ResourceNames::PrecipitationColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::PrecipitationColor));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto chrom = AddDeclStub(graph, "ChromAberrationPass");
    chrom->TestDeclareRead(std::string(ResourceNames::FogColor));
    chrom->TestDeclareWrite(std::string(ResourceNames::ChromAbColor));

    auto cg = AddDeclStub(graph, "ColorGradingPass");
    cg->TestDeclareRead(std::string(ResourceNames::ChromAbColor));
    cg->TestDeclareWrite(std::string(ResourceNames::ColorGradingColor));

    auto tonemap = AddDeclStub(graph, "ToneMapPass");
    tonemap->TestDeclareRead(std::string(ResourceNames::ColorGradingColor));
    tonemap->TestDeclareWrite(std::string(ResourceNames::ToneMapColor));

    auto vignette = AddDeclStub(graph, "VignettePass");
    vignette->TestDeclareRead(std::string(ResourceNames::ToneMapColor));
    vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    // No explicit edges at all — every ordering edge is derived from declarations.
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 28: full post-chain from PostProcess to Final must be "
           "hazard-free with only declaration-derived edges."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice28_FXAAAndSelectionOutlineVariantIsHazardFree)
{
    // When both FXAA and SelectionOutline are present the chain is:
    // Vignette → FXAA → SelectionOutline → UIComposite → Final.
    // UICompositePass declares reads on all three possible inputs;
    // the validator derives all ordering edges conservatively.
    RenderGraph graph;

    auto vignette = AddDeclStub(graph, "VignettePass");
    vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

    auto fxaa = AddDeclStub(graph, "FXAAPass");
    fxaa->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    fxaa->TestDeclareWrite(std::string(ResourceNames::FXAAColor));

    auto sel = AddDeclStub(graph, "SelectionOutlinePass");
    sel->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    sel->TestDeclareRead(std::string(ResourceNames::FXAAColor));
    sel->TestDeclareWrite(std::string(ResourceNames::SelectionOutlineColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    ui->TestDeclareRead(std::string(ResourceNames::FXAAColor));
    ui->TestDeclareRead(std::string(ResourceNames::SelectionOutlineColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 28: FXAA + SelectionOutline chain must be hazard-free with "
           "only declaration-derived edges."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice28_SelectionOutlineOnlyVariantIsHazardFree)
{
    // When FXAA is absent the chain is Vignette → SelectionOutline → UIComposite.
    // SelectionOutline's FXAAColor DeclareRead creates no derived edge because
    // no pass writes FXAAColor; only the VignetteColor pair matters.
    RenderGraph graph;

    auto vignette = AddDeclStub(graph, "VignettePass");
    vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

    auto sel = AddDeclStub(graph, "SelectionOutlinePass");
    sel->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    sel->TestDeclareRead(std::string(ResourceNames::FXAAColor)); // no producer — benign
    sel->TestDeclareWrite(std::string(ResourceNames::SelectionOutlineColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    ui->TestDeclareRead(std::string(ResourceNames::FXAAColor)); // no producer — benign
    ui->TestDeclareRead(std::string(ResourceNames::SelectionOutlineColor));
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 28: SelectionOutline-only (no FXAA) chain must derive "
           "Vignette→SelectionOutline→UIComposite from VignetteColor pair."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 29 — Particle → OITResolve → SSS → AOApply derived edges.
// ParticlePass DeclareWrite(OITAccum, OITRevealage) + OITResolvePass
// DeclareRead(OITAccum, OITRevealage) derives Particle→OITResolve.
// OITResolvePass DeclareWrite(SceneColor) + SSSPass DeclareRead(SceneColor)
// derives OITResolve→SSS.
// SSSPass DeclareWrite(SSSColor) + AOApplyPass DeclareRead(SSSColor)
// derives SSS→AOApply.
// No explicit AddExecutionDependency calls needed for these three edges.
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice29_ParticleToOITResolveDerivedEdge)
{
    // Minimal: ParticlePass writes OITAccum; OITResolvePass reads it.
    // The RAW pair must derive Particle → OITResolve without an explicit edge.
    RenderGraph graph;

    auto particle = AddDeclStub(graph, "ParticlePass");
    particle->TestDeclareWrite(std::string(ResourceNames::OITAccum));
    particle->TestDeclareWrite(std::string(ResourceNames::OITRevealage));
    particle->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto oit = AddDeclStub(graph, "OITResolvePass");
    oit->TestDeclareRead(std::string(ResourceNames::OITAccum));
    oit->TestDeclareRead(std::string(ResourceNames::OITRevealage));
    oit->TestDeclareRead(std::string(ResourceNames::SceneColor));
    oit->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 29: OITAccum DeclareWrite/DeclareRead must derive the "
           "ParticlePass → OITResolvePass ordering edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice29_OITResolveToSSSPassDerivedEdge)
{
    // OITResolvePass writes SceneColor; SSSRenderPass reads SceneColor.
    // The RAW pair must derive OITResolve → SSSPass without an explicit edge.
    RenderGraph graph;

    auto oit = AddDeclStub(graph, "OITResolvePass");
    oit->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto sss = AddDeclStub(graph, "SSSPass");
    sss->TestDeclareRead(std::string(ResourceNames::SceneColor));
    sss->TestDeclareWrite(std::string(ResourceNames::SSSColor));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SSSColor));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 29: SceneColor DeclareWrite/DeclareRead must derive "
           "OITResolvePass → SSSPass; SSSColor pair must derive SSS → AOApply."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice29_FullGeometryTailNoExplicitEdgesIsHazardFree)
{
    // Full chain from ParticlePass through AOApplyPass with NO explicit edges
    // after WaterPass→ParticlePass. Slice 29 derives all three RAW hops.
    RenderGraph graph;

    // Scene-color WAW chain still needs explicit edges (no DeclareWrite coverage
    // for FoliagePass, DecalPass, WaterPass in this slice).
    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto water = AddDeclStub(graph, "WaterPass");
    water->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto particle = AddDeclStub(graph, "ParticlePass");
    particle->TestDeclareWrite(std::string(ResourceNames::OITAccum));
    particle->TestDeclareWrite(std::string(ResourceNames::OITRevealage));
    particle->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto oit = AddDeclStub(graph, "OITResolvePass");
    oit->TestDeclareRead(std::string(ResourceNames::OITAccum));
    oit->TestDeclareRead(std::string(ResourceNames::OITRevealage));
    oit->TestDeclareRead(std::string(ResourceNames::SceneColor));
    oit->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto sss = AddDeclStub(graph, "SSSPass");
    sss->TestDeclareRead(std::string(ResourceNames::SceneColor));
    sss->TestDeclareWrite(std::string(ResourceNames::SSSColor));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneColor));
    aoApply->TestDeclareRead(std::string(ResourceNames::SSSColor));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    // Only WAW-ordering edges remain explicit; all RAW hops are derived.
    graph.AddExecutionDependency("ScenePass", "WaterPass");
    graph.AddExecutionDependency("WaterPass", "ParticlePass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 29: Particle→OITResolve→SSS→AOApply must all be hazard-free "
           "with only declaration-derived RAW edges."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 30 — SSAO/GTAO AOBuffer contracts.
// SSAOPass/GTAOPass DeclareWrite(AOBuffer) + AOApplyPass DeclareRead(AOBuffer)
// derive AO producer -> AOApply ordering. Legacy SSAO/GTAO -> Particle edges
// are no longer needed for AO correctness.
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice30_SSAOToAOApplyDerivedEdge)
{
    RenderGraph graph;

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    ssao->TestDeclareRead(std::string(ResourceNames::SceneNormals));
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 30: AOBuffer DeclareWrite/DeclareRead must derive "
           "SSAOPass -> AOApplyPass ordering."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice30_GTAOToAOApplyDerivedEdge)
{
    RenderGraph graph;

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    gtao->TestDeclareRead(std::string(ResourceNames::SceneNormals));
    gtao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 30: AOBuffer DeclareWrite/DeclareRead must derive "
           "GTAOPass -> AOApplyPass ordering."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice30_DualAOWritersNeedExplicitOrdering)
{
    RenderGraph graph;

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    // Mirrors production baseline: serialize AOBuffer dual writers explicitly.
    graph.AddExecutionDependency("SSAOPass", "GTAOPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 30: when both AO passes are present and declare AOBuffer writes, "
           "an explicit SSAOPass -> GTAOPass edge must serialize dual writers."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 31 — remove explicit WaterPass -> SSAO/GTAO edges.
// ScenePass DeclareWrite(SceneDepth) + SSAO/GTAO DeclareRead(SceneDepth)
// derive ScenePass -> AOPass ordering directly, so Water->AO edges are not
// required for hazard validation.
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice31_SceneToSSAODerivedEdgeWithoutWaterEdge)
{
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    // No explicit ScenePass->SSAOPass edge.
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 31: SceneDepth DeclareWrite/DeclareRead must derive "
           "ScenePass -> SSAOPass ordering without WaterPass->SSAOPass edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice31_SceneToGTAODerivedEdgeWithoutWaterEdge)
{
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    gtao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    // No explicit ScenePass->GTAOPass edge.
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 31: SceneDepth DeclareWrite/DeclareRead must derive "
           "ScenePass -> GTAOPass ordering without WaterPass->GTAOPass edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, MissingDecalToWaterEdgeIsFlagged)
{
    // After slice 32 the DecalPass→WaterPass ordering edge is DERIVED from the
    // SceneColor RMW declarations (DecalPass writes, WaterPass reads-then-writes).
    // This test validates the negative: if WaterPass only declares Write(SceneColor)
    // (no Read) and no explicit edge is present, the validator surfaces a WAW hazard.
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
    // After slice 32 the ScenePass→FoliagePass ordering edge is DERIVED from the
    // SceneColor RMW declaration (FoliagePass reads-then-writes SceneColor).
    // This test validates the negative: if FoliagePass only declares Write(SceneColor)
    // (no Read) and no explicit edge is present, the validator surfaces a WAW hazard.
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
    // After slice 32 the FoliagePass→DecalPass ordering edge is DERIVED from the
    // SceneColor RMW declaration (DecalPass reads-then-writes SceneColor).
    // This test validates the negative: if DecalPass only declares Write(SceneColor)
    // (no Read) and no explicit edge is present, the validator surfaces a WAW hazard.
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
    // After slice 32 the WaterPass→ParticlePass ordering edge is DERIVED from the
    // SceneColor RMW declaration (ParticlePass reads-then-writes SceneColor).
    // This test validates the negative: if the stubs only declare OIT writes (no
    // SceneColor RMW) and no explicit edge is present, the validator surfaces a
    // WAW hazard on OITAccum.
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

// =============================================================================
// Phase F slice 32 — geometry-chain derived edges.
// ScenePass→FoliagePass, FoliagePass→DecalPass, DecalPass→WaterPass, and
// WaterPass→ParticlePass are all derived from SceneColor RMW declarations.
// No explicit AddExecutionDependency calls are needed for these four edges.
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice32_SceneToFoliageDerivedFromSceneColor)
{
    // ScenePass.DeclareWrite(SceneColor) + FoliagePass.DeclareRead(SceneColor)
    // derives the ScenePass→FoliagePass RAW ordering edge.
    // No explicit AddExecutionDependency call required.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareRead(std::string(ResourceNames::SceneColor));
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("ScenePass", "FoliagePass");
    graph.AddExecutionDependency("FoliagePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 32: SceneColor DeclareWrite/DeclareRead must derive "
           "ScenePass -> FoliagePass ordering without an explicit edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice32_FoliageToDecalDerivedFromSceneColor)
{
    // FoliagePass.DeclareWrite(SceneColor) + DecalPass.DeclareRead(SceneColor)
    // derives the FoliagePass→DecalPass RAW ordering edge.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareRead(std::string(ResourceNames::SceneColor));
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto decal = AddDeclStub(graph, "DecalPass");
    decal->TestDeclareRead(std::string(ResourceNames::SceneColor));
    decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("FoliagePass", "DecalPass");
    graph.AddExecutionDependency("DecalPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 32: SceneColor DeclareWrite/DeclareRead must derive "
           "FoliagePass -> DecalPass ordering without an explicit edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice32_DecalToWaterDerivedFromSceneColor)
{
    // DecalPass.DeclareWrite(SceneColor) + WaterPass.DeclareRead(SceneColor)
    // derives the DecalPass→WaterPass RAW ordering edge.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareRead(std::string(ResourceNames::SceneColor));
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto decal = AddDeclStub(graph, "DecalPass");
    decal->TestDeclareRead(std::string(ResourceNames::SceneColor));
    decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto water = AddDeclStub(graph, "WaterPass");
    water->TestDeclareRead(std::string(ResourceNames::SceneColor));
    water->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("DecalPass", "WaterPass");
    graph.AddExecutionDependency("WaterPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 32: SceneColor DeclareWrite/DeclareRead must derive "
           "DecalPass -> WaterPass ordering without an explicit edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice32_WaterToParticleDerivedFromSceneColor)
{
    // WaterPass.DeclareWrite(SceneColor) + ParticlePass.DeclareRead(SceneColor)
    // derives the WaterPass→ParticlePass RAW ordering edge.
    RenderGraph graph;

    auto water = AddDeclStub(graph, "WaterPass");
    water->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto particle = AddDeclStub(graph, "ParticlePass");
    particle->TestDeclareRead(std::string(ResourceNames::SceneColor));
    particle->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("WaterPass", "ParticlePass");
    graph.AddExecutionDependency("ParticlePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 32: SceneColor DeclareWrite/DeclareRead must derive "
           "WaterPass -> ParticlePass ordering without an explicit edge."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 33 — deferred-path derived edges.
// All five deferred explicit edges are now derived from declaration pairs:
//   ScenePass→DeferredOpaqueDecalPass  — SceneDepth RAW
//   DeferredOpaqueDecalPass→DeferredLightingPass — SceneColor RAW
//   ScenePass→DeferredLightingPass (fallback) — SceneDepth RAW
//   DeferredLightingPass→ForwardOverlayPass — SceneColor RAW
//   ForwardOverlayPass→FoliagePass — SceneColor RAW (Foliage RMW from slice 32)
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice33_SceneToDeferredOpaqueDecalDerivedFromSceneDepth)
{
    // ScenePass.DeclareWrite(SceneDepth) + DeferredOpaqueDecalPass.DeclareRead(SceneDepth)
    // derives the ScenePass→DeferredOpaqueDecalPass RAW ordering edge.
    // No explicit AddExecutionDependency call required.
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
    EXPECT_TRUE(hazards.empty())
        << "Slice 33: SceneDepth DeclareWrite/DeclareRead must derive "
           "ScenePass -> DeferredOpaqueDecalPass ordering without an explicit edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice33_DeferredOpaqueDecalToDeferredLightingDerivedFromSceneColor)
{
    // DeferredOpaqueDecalPass.DeclareWrite(SceneColor) +
    // DeferredLightingPass.DeclareRead(SceneColor) derives the
    // DeferredOpaqueDecalPass→DeferredLightingPass RAW ordering edge.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto deferredDecal = AddDeclStub(graph, "DeferredOpaqueDecalPass");
    deferredDecal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    deferredDecal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto deferredLighting = AddDeclStub(graph, "DeferredLightingPass");
    deferredLighting->TestDeclareRead(std::string(ResourceNames::SceneColor));
    deferredLighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
    graph.AddExecutionDependency("DeferredLightingPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 33: SceneColor DeclareWrite/DeclareRead must derive "
           "DeferredOpaqueDecalPass -> DeferredLightingPass ordering without an explicit edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice33_DeferredLightingToForwardOverlayDerivedFromSceneColor)
{
    // DeferredLightingPass.DeclareWrite(SceneColor) +
    // ForwardOverlayPass.DeclareRead(SceneColor) derives the
    // DeferredLightingPass→ForwardOverlayPass RAW ordering edge.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto deferredLighting = AddDeclStub(graph, "DeferredLightingPass");
    deferredLighting->TestDeclareRead(std::string(ResourceNames::SceneColor));
    deferredLighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto forwardOverlay = AddDeclStub(graph, "ForwardOverlayPass");
    forwardOverlay->TestDeclareRead(std::string(ResourceNames::SceneColor));
    forwardOverlay->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("DeferredLightingPass", "ForwardOverlayPass");
    graph.AddExecutionDependency("ForwardOverlayPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 33: SceneColor DeclareWrite/DeclareRead must derive "
           "DeferredLightingPass -> ForwardOverlayPass ordering without an explicit edge."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice33_ForwardOverlayToFoliageDerivedFromSceneColor)
{
    // ForwardOverlayPass.DeclareWrite(SceneColor) + FoliagePass.DeclareRead(SceneColor)
    // derives the ForwardOverlayPass→FoliagePass RAW ordering edge.
    RenderGraph graph;

    auto forwardOverlay = AddDeclStub(graph, "ForwardOverlayPass");
    forwardOverlay->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareRead(std::string(ResourceNames::SceneColor));
    foliage->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("ForwardOverlayPass", "FoliagePass");
    graph.AddExecutionDependency("FoliagePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 33: SceneColor DeclareWrite/DeclareRead must derive "
           "ForwardOverlayPass -> FoliagePass ordering without an explicit edge."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F slice 27 — ShadowPass → ScenePass derived edge.
// Previously this edge required an explicit AddExecutionDependency call.
// Slice 27 derives the RAW edge automatically from the declaration pair.
// =============================================================================
TEST(RenderGraphConfigureTopology, MissingShadowToSceneExplicitEdge_DerivedEdgeSufficient)
{
    // Slice 27 behavior: ShadowPass.DeclareWrite(ShadowMapCSM) +
    // ScenePass.DeclareRead(ShadowMapCSM) derives the ordering edge.
    // No explicit AddExecutionDependency required.
    RenderGraph graph;

    auto shadow = AddDeclStub(graph, "ShadowPass");
    shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    // Intentionally omit: graph.AddExecutionDependency("ShadowPass", "ScenePass");
    // Slice 27 derives it from the DeclareWrite/DeclareRead pair.
    graph.AddExecutionDependency("ScenePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: ShadowPass→ScenePass ordering is derived from declarations, "
           "no explicit edge required."
        << HazardsToString(hazards);
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

// =============================================================================
// Phase F slice 27 — ScenePass → DeferredOpaqueDecalPass derived edge.
// Previously this edge required an explicit AddExecutionDependency call.
// Slice 27 derives the RAW edge from ScenePass.DeclareWrite(SceneDepth) +
// DeferredOpaqueDecalPass.DeclareRead(SceneDepth).
// =============================================================================
TEST(RenderGraphConfigureTopology, MissingSceneToDeferredOpaqueDecalExplicitEdge_DerivedEdgeSufficient)
{
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
    // Slice 27 derives the RAW edge from the SceneDepth declaration pair.
    graph.AddExecutionDependency("DeferredOpaqueDecalPass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "slice 27: ScenePass→DeferredOpaqueDecalPass ordering is derived from "
           "declarations, no explicit edge required."
        << HazardsToString(hazards);
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
    RegisterDeclarativeStubNode(barePathGraph, shadow);
    auto scene = Ref<DeclarativeStubPass>::Create("ScenePass");
    scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
    RegisterDeclarativeStubNode(barePathGraph, scene);
    auto lighting = Ref<DeclarativeStubPass>::Create("DeferredLightingPass");
    lighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
    lighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    lighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    RegisterDeclarativeStubNode(barePathGraph, lighting);
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
        RegisterDeclarativeStubNode(reordered, scene);

        auto lighting = Ref<DeclarativeStubPass>::Create("DeferredLightingPass");
        lighting->TestDeclareRead(std::string(ResourceNames::SceneNormals));
        lighting->TestDeclareRead(std::string(ResourceNames::SceneDepth));
        lighting->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        RegisterDeclarativeStubNode(reordered, lighting);

        auto decal = Ref<DeclarativeStubPass>::Create("DeferredOpaqueDecalPass");
        decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
        decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        RegisterDeclarativeStubNode(reordered, decal);

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
        RegisterDeclarativeStubNode(graph, shadow);

        auto scene = Ref<DeclarativeStubPass>::Create("ScenePass");
        scene->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
        scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
        // Mirror BuildPathTopology: ScenePass writes SceneNormals so the
        // Deferred branch's normals handoff into DeferredLightingPass /
        // DeferredOpaqueDecalPass actually has a declared producer edge
        // for the L5 validator to verify on each rebuild cycle.
        scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));
        RegisterDeclarativeStubNode(graph, scene);

        if (deferred)
        {
            auto deferredLight = Ref<DeclarativeStubPass>::Create("DeferredLightingPass");
            deferredLight->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            deferredLight->TestDeclareRead(std::string(ResourceNames::SceneNormals));
            deferredLight->TestDeclareWrite(std::string(ResourceNames::SceneColor));
            RegisterDeclarativeStubNode(graph, deferredLight);

            // Opaque-decal graph shim — mirrors ConfigureRenderGraph so
            // the rebuild test exercises the same pass set + edges as
            // production when switching back into Deferred. Resource
            // contract matches BuildPathTopology / DeferredOpaqueDecalPass::Init:
            // reads SceneDepth, writes SceneColor (does NOT read SceneNormals).
            auto decal = Ref<DeclarativeStubPass>::Create("DeferredOpaqueDecalPass");
            decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
            decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));
            RegisterDeclarativeStubNode(graph, decal);

            graph.AddExecutionDependency("ScenePass", "DeferredOpaqueDecalPass");
            graph.AddExecutionDependency("DeferredOpaqueDecalPass", "DeferredLightingPass");
        }

        auto final = Ref<DeclarativeStubPass>::Create("FinalPass");
        final->TestDeclareRead(std::string(ResourceNames::SceneColor));
        RegisterDeclarativeStubNode(graph, final);

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
        EXPECT_EQ(graph.GetPassSubmissionInfo().size(), expectedPassCount)
            << "Cycle " << i << ": residual passes from prior cycle";
    }
}

// =============================================================================
// Phase F slice 34 — conditional AO pass registration eliminates the last
// remaining explicit forward-path execution-dependency edge.
//
// Before slice 34: both SSAOPass and GTAOPass were always registered in
// ConfigureRenderGraph and an explicit AddExecutionDependency("SSAOPass",
// "GTAOPass") serialised the dual AOBuffer writers (WAW).
//
// After slice 34: ConfigureRenderGraph registers only the pass corresponding
// to PostProcessSettings::ActiveAOTechnique. With at most one AOBuffer writer
// in the graph the WAW cannot occur and no explicit edge is required.
// ApplyRendererSettings detects ActiveAOTechnique changes and calls
// ConfigureRenderGraph to rebuild the topology with the new single-pass set.
// =============================================================================

TEST(RenderGraphConfigureTopology, Slice34_DualAOWritersWithoutExplicitEdgeIsWAWHazard)
{
    // Negative test: documents the hazard that existed before slice 34.
    // When both SSAOPass and GTAOPass are in the graph and both declare an
    // AOBuffer write without an explicit ordering edge, ValidateResourceHazards
    // must flag a WAW on AOBuffer. Slice 34 avoids this by never registering
    // both passes simultaneously.
    RenderGraph graph;

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    // Intentionally omit: graph.AddExecutionDependency("SSAOPass", "GTAOPass");
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_FALSE(hazards.empty())
        << "Slice 34: two AOBuffer writers without an explicit ordering edge "
           "must produce a WAW hazard — the pre-slice-34 topology required an "
           "explicit SSAOPass -> GTAOPass edge to suppress it.";
}

TEST(RenderGraphConfigureTopology, Slice34_SSAOOnlyInGraphHasNoAOBufferWAW)
{
    // Positive test: AOTechnique::SSAO — only SSAOPass is registered; no WAW
    // on AOBuffer and no explicit ordering edge needed.
    RenderGraph graph;

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 34: SSAOPass alone writes AOBuffer — single writer, no WAW, "
           "no explicit edge required.  "
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice34_GTAOOnlyInGraphHasNoAOBufferWAW)
{
    // Positive test: AOTechnique::GTAO — only GTAOPass is registered; no WAW
    // on AOBuffer and no explicit ordering edge needed.
    RenderGraph graph;

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 34: GTAOPass alone writes AOBuffer — single writer, no WAW, "
           "no explicit edge required.  "
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice34_NoneAOTechniqueNoAOPassInGraphIsHazardFree)
{
    // Positive test: AOTechnique::None — neither SSAO nor GTAO is registered.
    // AOBuffer has no writer; the graph is trivially hazard-free on that
    // resource. AOApplyPass reads a zero/black texture at runtime when AO
    // is disabled, but the validator only reasons about declared producers.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));

    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 34: with no AO passes registered (AOTechnique::None), "
           "the graph must be hazard-free — no AOBuffer writer exists."
        << HazardsToString(hazards);
}

// =============================================================================
// Phase F — Slice 35: Self-resolving input handles (OITResolve & SSS)
//
// Slice 35 adds `RGCommandContext::GetBlackboard()` so passes can look up
// their own input handles from the FrameBlackboard during Execute() instead
// of relying on a per-frame `SetInputFramebufferHandle` side-channel call
// from EndScene().  The first two tests are pure API/unit tests; the third
// and fourth are topology regression tests confirming the static declarations
// on OITResolvePass and SSSRenderPass still yield the correct hazard-free
// ordering after the side-channel was removed.
// =============================================================================

TEST(RGCommandContextBlackboard, Slice35_GetBlackboardReturnsNullptrWithoutGraph)
{
    // A default-constructed context has no attached render graph.
    // GetBlackboard() must return nullptr rather than crashing or returning
    // a dangling pointer — this is the headless / unit-test fallback guard.
    RGCommandContext context;
    EXPECT_EQ(context.GetBlackboard(), nullptr)
        << "Slice 35: GetBlackboard() must return nullptr when no render graph "
           "is attached (headless / unit-test mode).";
}

TEST(RGCommandContextBlackboard, Slice35_GetBlackboardReturnsGraphBlackboardWhenAttached)
{
    // When a render graph is attached, GetBlackboard() must return a non-null
    // pointer to the graph's FrameBlackboard.  A freshly created graph has
    // all handles invalid (unset), so SceneColor.IsValid() should be false.
    RenderGraph graph;
    RGCommandContext context;
    context.SetRenderGraph(&graph);

    const FrameBlackboard* board = context.GetBlackboard();
    ASSERT_NE(board, nullptr)
        << "Slice 35: GetBlackboard() must return the graph's blackboard "
           "when a render graph is attached.";
    EXPECT_FALSE(board->SceneColor.IsValid())
        << "Freshly-constructed blackboard should have no populated handles.";
}

TEST(RenderGraphConfigureTopology, Slice35_OITResolveAndSSSOrderingDerivesFromDeclarations)
{
    // OITResolvePass declares: Read(OITAccum), Read(OITRevealage),
    //                          Read(SceneColor), Write(SceneColor)  [RMW]
    // SSSRenderPass  declares: Read(SceneColor), Write(SSSColor)
    //
    // The SceneColor RMW on OITResolve creates a RAW edge to SSSPass
    // (because SSS reads SceneColor that OITResolve wrote).  Slice 35
    // removes the per-frame SetInputFramebufferHandle side-channel; this
    // test confirms the static declarations alone keep the topology
    // hazard-free.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto particle = AddDeclStub(graph, "ParticlePass");
    particle->TestDeclareWrite(std::string(ResourceNames::OITAccum));
    particle->TestDeclareWrite(std::string(ResourceNames::OITRevealage));

    auto oitResolve = AddDeclStub(graph, "OITResolvePass");
    oitResolve->TestDeclareRead(std::string(ResourceNames::OITAccum));
    oitResolve->TestDeclareRead(std::string(ResourceNames::OITRevealage));
    oitResolve->TestDeclareRead(std::string(ResourceNames::SceneColor));
    oitResolve->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto sss = AddDeclStub(graph, "SSSPass");
    sss->TestDeclareRead(std::string(ResourceNames::SceneColor));
    sss->TestDeclareWrite(std::string(ResourceNames::SSSColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SSSColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 35: OITResolve → SSS ordering derives from SceneColor RAW "
           "declaration — no explicit edge or per-frame side-channel needed."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice35_SSSColorRAWEdgeToAOApplyDerivesFromDeclarations)
{
    // SSSRenderPass  declares: Read(SceneColor), Write(SSSColor)
    // AOApplyRenderPass declares: Read(SSSColor), Write(AOApplyColor)
    //
    // The SSSColor RAW edge derives the SSS → AOApplyPass ordering without
    // any explicit side-channel, validating the slice 35 contracts.
    RenderGraph graph;

    auto oitResolve = AddDeclStub(graph, "OITResolvePass");
    oitResolve->TestDeclareRead(std::string(ResourceNames::SceneColor));
    oitResolve->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto sss = AddDeclStub(graph, "SSSPass");
    sss->TestDeclareRead(std::string(ResourceNames::SceneColor));
    sss->TestDeclareWrite(std::string(ResourceNames::SSSColor));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SSSColor));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 35: SSS → AOApplyPass ordering derives from SSSColor RAW "
           "declaration — no side-channel needed."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Slice 36 — Self-resolving SceneColor/SceneDepth for 5 forward geometry passes
//
// ForwardOverlayPass, FoliagePass, WaterPass, DecalPass, and ParticlePass
// now look up SceneColor (and SceneDepth for Decal) from the blackboard
// directly inside Execute(). The side-channel SetSceneColorHandle /
// SetSceneDepthHandle setters have been removed. These tests verify that the
// resulting declaration topology produces no hazards and that the known
// RAW edges (SceneColor writer → each consumer) are derived purely from
// declarations.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, Slice36_ForwardGeometryPassesSceneColorRAWEdgeFromDeclarations)
{
    // SceneRenderPass writes SceneColor; the 5 forward geometry passes read
    // it. The RAW edges must be inferred from declarations alone — no
    // side-channel setter is involved.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    for (const char* name : { "ForwardOverlayPass", "FoliagePass", "WaterPass",
                              "DecalPass", "ParticlePass" })
    {
        auto p = AddDeclStub(graph, name);
        p->TestDeclareRead(std::string(ResourceNames::SceneColor));
    }

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 36: all 5 forward geometry passes read SceneColor with no "
           "concurrent writer — no hazards expected."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice36_DecalPassSceneDepthRAWEdgeFromDeclarations)
{
    // DecalPass reads SceneDepth for projection. The RAW edge from the
    // scene pass writer must be derived from declarations; no
    // SetSceneDepthHandle side-channel is needed.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto decal = AddDeclStub(graph, "DecalPass");
    decal->TestDeclareRead(std::string(ResourceNames::SceneColor));
    decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 36: DecalPass reads SceneDepth — RAW edge inferred from "
           "declarations; no side-channel setter required."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice36_ParticleAndWaterAfterSceneColorWriter)
{
    // Regression: ParticlePass and WaterPass both read SceneColor written
    // by ScenePass — ordering must derive from declarations alone.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto water = AddDeclStub(graph, "WaterPass");
    water->TestDeclareRead(std::string(ResourceNames::SceneColor));

    auto particle = AddDeclStub(graph, "ParticlePass");
    particle->TestDeclareRead(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 36: ParticlePass and WaterPass read SceneColor written by "
           "ScenePass — no hazards when ordering derives from declarations."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice36_FoliageAndOverlayAfterSceneColorWriter)
{
    // Regression: FoliagePass and ForwardOverlayPass read SceneColor. This
    // verifies that removing the SetSceneColorHandle setter does not break
    // the declaration-derived topology for these two passes.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto foliage = AddDeclStub(graph, "FoliagePass");
    foliage->TestDeclareRead(std::string(ResourceNames::SceneColor));

    auto overlay = AddDeclStub(graph, "ForwardOverlayPass");
    overlay->TestDeclareRead(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 36: FoliagePass and ForwardOverlayPass read SceneColor — "
           "ordering derives from declarations with no side-channel setter."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Phase H follow-up — DecalRenderPass raw depth fallback removed
//
// DecalRenderPass previously fell back to
// `m_SceneFramebuffer->GetDepthAttachmentRendererID()` when the blackboard
// SceneDepth resolve returned 0. That fallback is now removed: the pass
// self-resolves exclusively from `board->SceneDepth`, matching the same
// blackboard-only pattern used by SSAORenderPass and GTAORenderPass.
//
// This test verifies that the declaration-derived topology still produces
// the correct ScenePass → DecalPass RAW edge for both SceneColor and
// SceneDepth with zero side-channel setters.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, PhaseH_DecalPassSceneDepthNoRawFallback)
{
    // ScenePass writes SceneDepth + SceneColor.
    // DecalPass reads both (SceneDepth for projection, SceneColor as render target).
    // The two RAW edges must be derived from declarations; no raw framebuffer
    // fallback should be needed.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto decal = AddDeclStub(graph, "DecalPass");
    decal->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    decal->TestDeclareRead(std::string(ResourceNames::SceneColor));
    decal->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Phase H: DecalPass resolves SceneDepth purely from the blackboard "
           "— no raw framebuffer fallback needed; ordering is hazard-free."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Slice 37 — Self-resolving SceneDepth/SceneNormals for SSAO and GTAO passes
//
// SSAORenderPass and GTAORenderPass now look up SceneDepth and SceneNormals
// from the blackboard directly inside Execute(). The side-channel
// SetSceneDepthHandle / SetSceneNormalsHandle setters have been removed from
// both passes. These tests verify the declaration-derived topology produces
// no hazards and that the AO passes are ordered after the scene writer.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, Slice37_SSAOPassSelfResolvesSceneDepthAndNormals)
{
    // SSAORenderPass declares: Read(SceneDepth), Read(SceneNormals).
    // SceneRenderPass declares: Write(SceneDepth), Write(SceneNormals).
    // The two RAW edges must derive execution order without any side-channel.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    ssao->TestDeclareRead(std::string(ResourceNames::SceneNormals));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 37: SSAOPass reads SceneDepth+SceneNormals — ordering "
           "derives from declarations; no SetSceneDepthHandle side-channel needed."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice37_GTAOPassSelfResolvesSceneDepthAndNormals)
{
    // Same contract as SSAO but for GTAORenderPass.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    gtao->TestDeclareRead(std::string(ResourceNames::SceneNormals));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 37: GTAOPass reads SceneDepth+SceneNormals — ordering "
           "derives from declarations; no SetSceneDepthHandle side-channel needed."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice37_AOPassesAfterSceneDepthWriterNoHazards)
{
    // Both SSAO and GTAO reading SceneDepth/SceneNormals written by the scene
    // pass must produce no concurrent-write hazards.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneNormals));

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    ssao->TestDeclareRead(std::string(ResourceNames::SceneNormals));

    auto gtao = AddDeclStub(graph, "GTAOPass");
    gtao->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    gtao->TestDeclareRead(std::string(ResourceNames::SceneNormals));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 37: SSAOPass and GTAOPass both reading SceneDepth+SceneNormals "
           "— no WAW hazard (neither writes those resources)."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Phase F — Slice 38: AOApplyRenderPass self-resolves its three blackboard inputs.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, Slice38_AOApplyPassSelfResolvesSceneColor)
{
    // AOApplyPass reads SceneColor (written by the scene pass); because the pass
    // self-resolves via GetBlackboard() there must be no concurrent-write hazard
    // between the scene-pass writer and the AOApplyPass reader.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 38: AOApplyPass reading SceneColor written by ScenePass — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice38_AOApplyPassSelfResolvesAOBufferAndSceneDepth)
{
    // AOApplyPass reads both AOBuffer (written by SSAO/GTAO) and SceneDepth
    // (written by the scene pass).  Neither resource is written by AOApplyPass
    // itself so there must be no concurrent-write hazard.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 38: AOApplyPass reading AOBuffer+SceneDepth — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice38_AOApplyPassPrefersSSSColorOverSceneColor)
{
    // When SSSColor is produced upstream, AOApplyPass should pick it up as its
    // input and the dependency chain SSS->AOApply must be hazard-free.
    RenderGraph graph;

    auto sss = AddDeclStub(graph, "SSSPass");
    sss->TestDeclareWrite(std::string(ResourceNames::SSSColor));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SSSColor));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 38: AOApplyPass reading SSSColor — no hazard."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Phase F — Slice 39: PostProcessRenderPass self-resolves its five blackboard inputs.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, Slice39_PostProcessPassSelfResolvesInputChain)
{
    // PostProcessPass reads the most downstream color source: AOApplyColor
    // (when written) else SSSColor else SceneColor.  When AOApplyColor is
    // present the dependency must be hazard-free.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneColor));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto postProcess = AddDeclStub(graph, "PostProcessPass");
    postProcess->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    postProcess->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 39: PostProcessPass reading AOApplyColor — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice39_PostProcessPassSelfResolvesSceneDepthAndAOBuffer)
{
    // PostProcessPass reads SceneDepth and AOBuffer — neither is written by
    // PostProcessPass itself so no concurrent-write hazard should exist.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto postProcess = AddDeclStub(graph, "PostProcessPass");
    postProcess->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    postProcess->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    postProcess->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 39: PostProcessPass reading SceneDepth+AOBuffer — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice39_PostProcessPassSelfResolvesShadowMapAndVelocity)
{
    // PostProcessPass also reads ShadowMapCSM and the Velocity buffer.
    // Both are written by passes earlier in the frame; no WAW hazard expected.
    RenderGraph graph;

    auto shadow = AddDeclStub(graph, "ShadowPass");
    shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::Velocity));

    auto postProcess = AddDeclStub(graph, "PostProcessPass");
    postProcess->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    postProcess->TestDeclareRead(std::string(ResourceNames::Velocity));
    postProcess->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 39: PostProcessPass reading ShadowMapCSM+Velocity — no hazard."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Phase F — Slice 40: DOFRenderPass, MotionBlurRenderPass, TAARenderPass
//           self-resolve their blackboard inputs.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, Slice40_DOFPassSelfResolvesInputAndSceneDepth)
{
    // DOFPass reads BloomColor (if present) else PostProcessColor, plus SceneDepth.
    // No concurrent writes — no hazard expected.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto dof = AddDeclStub(graph, "DOFPass");
    dof->TestDeclareRead(std::string(ResourceNames::BloomColor));
    dof->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    dof->TestDeclareWrite(std::string(ResourceNames::DOFColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::DOFColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 40: DOFPass reading BloomColor+SceneDepth — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice40_MotionBlurPassSelfResolvesInputChain)
{
    // MotionBlurPass prefers DOFColor > BloomColor > PostProcessColor, plus SceneDepth.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto dof = AddDeclStub(graph, "DOFPass");
    dof->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    dof->TestDeclareWrite(std::string(ResourceNames::DOFColor));

    auto motionBlur = AddDeclStub(graph, "MotionBlurPass");
    motionBlur->TestDeclareRead(std::string(ResourceNames::DOFColor));
    motionBlur->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    motionBlur->TestDeclareWrite(std::string(ResourceNames::MotionBlurColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::MotionBlurColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 40: MotionBlurPass reading DOFColor+SceneDepth — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice40_TAAPassSelfResolvesInputDepthAndVelocity)
{
    // TAAPass prefers MotionBlurColor > DOFColor > BloomColor > PostProcessColor,
    // plus SceneDepth and Velocity.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::Velocity));
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto motionBlur = AddDeclStub(graph, "MotionBlurPass");
    motionBlur->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    motionBlur->TestDeclareWrite(std::string(ResourceNames::MotionBlurColor));

    auto taa = AddDeclStub(graph, "TAAPass");
    taa->TestDeclareRead(std::string(ResourceNames::MotionBlurColor));
    taa->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    taa->TestDeclareRead(std::string(ResourceNames::Velocity));
    taa->TestDeclareWrite(std::string(ResourceNames::TAAColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::TAAColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 40: TAAPass reading MotionBlurColor+SceneDepth+Velocity — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice41_DeferredLightingPassSelfResolvesSceneColorAndGBuffer)
{
    // DeferredLightingPass only runs in Deferred rendering mode. It reads
    // GBuffer attachments (albedo, normal, emissive) and SceneDepth, then
    // writes to SceneColor. Phase F slice 41 eliminates all per-frame
    // SetXxx() handle methods; Execute() now self-resolves via the
    // render-graph blackboard.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::GBufferAlbedo));
    scene->TestDeclareWrite(std::string(ResourceNames::GBufferNormal));
    scene->TestDeclareWrite(std::string(ResourceNames::GBufferEmissive));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto deferred = AddDeclStub(graph, "DeferredLightingPass");
    deferred->TestDeclareRead(std::string(ResourceNames::GBufferAlbedo));
    deferred->TestDeclareRead(std::string(ResourceNames::GBufferNormal));
    deferred->TestDeclareRead(std::string(ResourceNames::GBufferEmissive));
    deferred->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    deferred->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 41: DeferredLightingPass reading GBuffer+SceneDepth — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice41_DeferredLightingPassSelfResolvesSceneDepth)
{
    // DeferredLightingPass self-resolves SceneDepth from the render-graph
    // blackboard. Ensure execution dependencies are correctly validated.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));

    auto deferred = AddDeclStub(graph, "DeferredLightingPass");
    deferred->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    deferred->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 41: DeferredLightingPass reading SceneDepth — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice41_DeferredLightingPassSelfResolvesMSAAVariants)
{
    // DeferredLightingPass supports MSAA variants: when sample count > 1,
    // it self-resolves the MSAA attachment variants
    // (GBufferAlbedoMS, GBufferNormalMS, GBufferEmissiveMS, SceneDepthMS)
    // from the render-graph blackboard. Ensure read/write hazards are
    // correctly validated for the MSAA path.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::GBufferAlbedoMS));
    scene->TestDeclareWrite(std::string(ResourceNames::GBufferNormalMS));
    scene->TestDeclareWrite(std::string(ResourceNames::GBufferEmissiveMS));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepthMS));

    auto deferred = AddDeclStub(graph, "DeferredLightingPass");
    deferred->TestDeclareRead(std::string(ResourceNames::GBufferAlbedoMS));
    deferred->TestDeclareRead(std::string(ResourceNames::GBufferNormalMS));
    deferred->TestDeclareRead(std::string(ResourceNames::GBufferEmissiveMS));
    deferred->TestDeclareRead(std::string(ResourceNames::SceneDepthMS));
    deferred->TestDeclareWrite(std::string(ResourceNames::SceneColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SceneColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 41: DeferredLightingPass reading GBufferMS+SceneDepthMS — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice42_FogPassSelfResolvesInputAndSceneDepth)
{
    // FogPass reads the upstream post-process result (with preference chain:
    // PrecipitationColor > TAAColor > MotionBlurColor > DOFColor > BloomColor >
    // PostProcessColor) and SceneDepth. Phase F slice 42 eliminates the per-frame
    // SetInputFramebufferHandle and SetSceneDepthTextureHandle methods;
    // Execute() now self-resolves via the render-graph blackboard.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    fog->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::FogColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 42: FogPass reading PostProcessColor+SceneDepth — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice42_FogPassSelfResolvesShadowMapCSM)
{
    // FogPass optionally self-resolves ShadowMapCSM from the render-graph
    // blackboard for shadowing the fog effect. The shadow map is optional
    // (fallback to raw ID when not available).
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    fog->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    fog->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::FogColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 42: FogPass reading PostProcessColor+SceneDepth+ShadowMapCSM — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice42_FogPassSelfResolvesUpstreamChain)
{
    // FogPass prefers downstream post-process results when available:
    // Precipitation > TAA > MotionBlur > DOF > Bloom > PostProcess.
    // Ensure the execution dependency is correctly validated when the
    // entire chain is present.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto dof = AddDeclStub(graph, "DOFPass");
    dof->TestDeclareRead(std::string(ResourceNames::BloomColor));
    dof->TestDeclareWrite(std::string(ResourceNames::DOFColor));

    auto mb = AddDeclStub(graph, "MotionBlurPass");
    mb->TestDeclareRead(std::string(ResourceNames::DOFColor));
    mb->TestDeclareWrite(std::string(ResourceNames::MotionBlurColor));

    auto taa = AddDeclStub(graph, "TAAPass");
    taa->TestDeclareRead(std::string(ResourceNames::MotionBlurColor));
    taa->TestDeclareWrite(std::string(ResourceNames::TAAColor));

    auto precip = AddDeclStub(graph, "PrecipitationPass");
    precip->TestDeclareRead(std::string(ResourceNames::TAAColor));
    precip->TestDeclareWrite(std::string(ResourceNames::PrecipitationColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::PrecipitationColor)); // Prefers this
    fog->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::FogColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 42: FogPass reading PrecipitationColor upstream chain — no hazard."
        << HazardsToString(hazards);
}

// ---------------------------------------------------------------------------
// Phase H — Follow-up: AO/DOF/MotionBlur/TAA/Fog no longer keep raw
// depth/shadow fallback IDs. The blackboard declarations alone must drive the
// full post-process dependency chain.
// ---------------------------------------------------------------------------

TEST(RenderGraphConfigureTopology, PhaseH_PostChainDepthAndShadowUsersNeedNoRawFallbackIDs)
{
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::SceneColor));
    scene->TestDeclareWrite(std::string(ResourceNames::SceneDepth));
    scene->TestDeclareWrite(std::string(ResourceNames::Velocity));

    auto shadow = AddDeclStub(graph, "ShadowPass");
    shadow->TestDeclareWrite(std::string(ResourceNames::ShadowMapCSM));

    auto ssao = AddDeclStub(graph, "SSAOPass");
    ssao->TestDeclareWrite(std::string(ResourceNames::AOBuffer));

    auto aoApply = AddDeclStub(graph, "AOApplyPass");
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneColor));
    aoApply->TestDeclareRead(std::string(ResourceNames::AOBuffer));
    aoApply->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    aoApply->TestDeclareWrite(std::string(ResourceNames::AOApplyColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::AOApplyColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto dof = AddDeclStub(graph, "DOFPass");
    dof->TestDeclareRead(std::string(ResourceNames::BloomColor));
    dof->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    dof->TestDeclareWrite(std::string(ResourceNames::DOFColor));

    auto motionBlur = AddDeclStub(graph, "MotionBlurPass");
    motionBlur->TestDeclareRead(std::string(ResourceNames::DOFColor));
    motionBlur->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    motionBlur->TestDeclareWrite(std::string(ResourceNames::MotionBlurColor));

    auto taa = AddDeclStub(graph, "TAAPass");
    taa->TestDeclareRead(std::string(ResourceNames::MotionBlurColor));
    taa->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    taa->TestDeclareRead(std::string(ResourceNames::Velocity));
    taa->TestDeclareWrite(std::string(ResourceNames::TAAColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::TAAColor));
    fog->TestDeclareRead(std::string(ResourceNames::SceneDepth));
    fog->TestDeclareRead(std::string(ResourceNames::ShadowMapCSM));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::FogColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Phase H follow-up: AO/DOF/MotionBlur/TAA/Fog should derive their full depth/shadow ordering from blackboard reads only; no raw fallback texture IDs required."
        << HazardsToString(hazards);
}

// Slice 43 — ColorGradingRenderPass, BloomRenderPass, ChromaticAberrationRenderPass,
// VignetteRenderPass, ToneMapRenderPass self-resolve input framebuffer from blackboard.

TEST(RenderGraphConfigureTopology, Slice43_BloomPassSelfResolvesPostProcessColor)
{
    // BloomPass reads PostProcessColor (sole input choice).
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::BloomColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 43: BloomPass self-resolving PostProcessColor — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice43_ChromaticAberrationPassSelfResolvesUpstreamChain)
{
    // ChromaticAberrationPass uses 7-step preference chain: Fog > Precip > TAA > MB > DOF > Bloom > PostProcess.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::BloomColor));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto chromAb = AddDeclStub(graph, "ChromaticAberrationPass");
    chromAb->TestDeclareRead(std::string(ResourceNames::FogColor)); // Prefers this
    chromAb->TestDeclareWrite(std::string(ResourceNames::ChromAbColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::ChromAbColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 43: ChromaticAberrationPass self-resolving FogColor upstream — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice43_ColorGradingPassSelfResolvesUpstreamChain)
{
    // ColorGradingPass uses 7-step preference chain: Fog > Precip > TAA > MB > DOF > Bloom > PostProcess.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto fog = AddDeclStub(graph, "FogPass");
    fog->TestDeclareRead(std::string(ResourceNames::BloomColor));
    fog->TestDeclareWrite(std::string(ResourceNames::FogColor));

    auto chromAb = AddDeclStub(graph, "ChromaticAberrationPass");
    chromAb->TestDeclareRead(std::string(ResourceNames::FogColor));
    chromAb->TestDeclareWrite(std::string(ResourceNames::ChromAbColor));

    auto colorGrad = AddDeclStub(graph, "ColorGradingPass");
    colorGrad->TestDeclareRead(std::string(ResourceNames::ChromAbColor)); // Prefers this
    colorGrad->TestDeclareWrite(std::string(ResourceNames::ColorGradingColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::ColorGradingColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 43: ColorGradingPass self-resolving ChromAbColor upstream — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice43_ToneMapAndVignettePassSelfResolveUpstream)
{
    // ToneMap and Vignette each use 7-step preference chain from the post-process chain.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto colorGrad = AddDeclStub(graph, "ColorGradingPass");
    colorGrad->TestDeclareRead(std::string(ResourceNames::BloomColor));
    colorGrad->TestDeclareWrite(std::string(ResourceNames::ColorGradingColor));

    auto toneMap = AddDeclStub(graph, "ToneMapPass");
    toneMap->TestDeclareRead(std::string(ResourceNames::ColorGradingColor)); // Prefers this
    toneMap->TestDeclareWrite(std::string(ResourceNames::ToneMapColor));

    auto vignette = AddDeclStub(graph, "VignettePass");
    vignette->TestDeclareRead(std::string(ResourceNames::ToneMapColor)); // Prefers this
    vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::VignetteColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 43: ToneMap and Vignette self-resolving upstream chain — no hazard."
        << HazardsToString(hazards);
}

// Slice 44 — FXAARenderPass, PrecipitationRenderPass, SelectionOutlineRenderPass,
// UICompositeRenderPass, FinalRenderPass self-resolve input framebuffer from blackboard.

TEST(RenderGraphConfigureTopology, Slice44_FXAAPassSelfResolvesUpstreamChain)
{
    // FXAAPass uses 11-step preference chain: Vignette > ToneMap > ColorGrading >
    // ChromAb > Fog > Precip > TAA > MB > DOF > Bloom > PostProcess.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto vignette = AddDeclStub(graph, "VignettePass");
    vignette->TestDeclareRead(std::string(ResourceNames::BloomColor));
    vignette->TestDeclareWrite(std::string(ResourceNames::VignetteColor));

    auto fxaa = AddDeclStub(graph, "FXAAPass");
    fxaa->TestDeclareRead(std::string(ResourceNames::VignetteColor)); // Prefers this
    fxaa->TestDeclareWrite(std::string(ResourceNames::FXAAColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::FXAAColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 44: FXAAPass self-resolving VignetteColor upstream — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice44_PrecipitationPassSelfResolvesUpstreamChain)
{
    // PrecipitationPass uses 5-step preference chain: TAA > MotionBlur > DOF > Bloom > PostProcess.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto bloom = AddDeclStub(graph, "BloomPass");
    bloom->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    bloom->TestDeclareWrite(std::string(ResourceNames::BloomColor));

    auto precip = AddDeclStub(graph, "PrecipitationPass");
    precip->TestDeclareRead(std::string(ResourceNames::BloomColor)); // Prefers this
    precip->TestDeclareWrite(std::string(ResourceNames::PrecipitationColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::PrecipitationColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 44: PrecipitationPass self-resolving BloomColor upstream — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice44_SelectionOutlinePassSelfResolvesUpstreamChain)
{
    // SelectionOutlinePass uses 12-step preference chain: FXAA > Vignette > ToneMap > ...
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto fxaa = AddDeclStub(graph, "FXAAPass");
    fxaa->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    fxaa->TestDeclareWrite(std::string(ResourceNames::FXAAColor));

    auto selection = AddDeclStub(graph, "SelectionOutlinePass");
    selection->TestDeclareRead(std::string(ResourceNames::FXAAColor)); // Prefers this
    selection->TestDeclareWrite(std::string(ResourceNames::SelectionOutlineColor));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::SelectionOutlineColor));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 44: SelectionOutlinePass self-resolving FXAAColor upstream — no hazard."
        << HazardsToString(hazards);
}

TEST(RenderGraphConfigureTopology, Slice44_UICompositeAndFinalPassSelfResolveUpstream)
{
    // UICompositePass uses 13-step preference chain starting with SelectionOutline.
    // FinalPass reads UIComposite.
    RenderGraph graph;

    auto scene = AddDeclStub(graph, "ScenePass");
    scene->TestDeclareWrite(std::string(ResourceNames::PostProcessColor));

    auto fxaa = AddDeclStub(graph, "FXAAPass");
    fxaa->TestDeclareRead(std::string(ResourceNames::PostProcessColor));
    fxaa->TestDeclareWrite(std::string(ResourceNames::FXAAColor));

    auto ui = AddDeclStub(graph, "UICompositePass");
    ui->TestDeclareRead(std::string(ResourceNames::FXAAColor)); // Prefers this
    ui->TestDeclareWrite(std::string(ResourceNames::UIComposite));

    auto final = AddDeclStub(graph, "FinalPass");
    final->TestDeclareRead(std::string(ResourceNames::UIComposite));
    graph.SetFinalPass("FinalPass");

    const auto hazards = graph.ValidateResourceHazards();
    EXPECT_TRUE(hazards.empty())
        << "Slice 44: UIComposite and FinalPass self-resolving upstream chain — no hazard."
        << HazardsToString(hazards);
}
