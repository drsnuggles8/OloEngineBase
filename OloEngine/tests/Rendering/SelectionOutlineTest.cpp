#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"

#include <glm/glm.hpp>
#include <cstring>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Minimal Stub RenderPass (no GL)
// =============================================================================

class OutlineStubPass : public RenderPass
{
  public:
    explicit OutlineStubPass(const std::string& name)
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
};

static Ref<OutlineStubPass> AddStub(RenderGraph& graph, const std::string& name)
{
    auto pass = Ref<OutlineStubPass>::Create(name);
    pass->SetName(name);
    graph.AddPass(pass);
    return pass;
}

// =============================================================================
// SelectionOutlineUBO Layout Tests
// =============================================================================

TEST(SelectionOutlineUBO, SizeIs304Bytes)
{
    EXPECT_EQ(UBOStructures::SelectionOutlineUBO::GetSize(), 304u);
    EXPECT_EQ(sizeof(UBOStructures::SelectionOutlineUBO), 304u);
}

TEST(SelectionOutlineUBO, SizeIs16ByteAligned)
{
    EXPECT_EQ(sizeof(UBOStructures::SelectionOutlineUBO) % 16, 0u);
}

TEST(SelectionOutlineUBO, MaxSelectedEntitiesIs64)
{
    EXPECT_EQ(UBOStructures::SelectionOutlineUBO::MaxSelectedEntities, 64u);
}

TEST(SelectionOutlineUBO, FieldOffsets_Std140Compatible)
{
    // Row 0: OutlineColor (vec4) — offset 0, size 16
    EXPECT_EQ(offsetof(UBOStructures::SelectionOutlineUBO, OutlineColor), 0u);

    // Row 1: TexelSize (vec4) — offset 16, size 16
    EXPECT_EQ(offsetof(UBOStructures::SelectionOutlineUBO, TexelSize), 16u);

    // Row 2: SelectedCount (int) + OutlineWidth (int) + 2x pad — offset 32, size 16
    EXPECT_EQ(offsetof(UBOStructures::SelectionOutlineUBO, SelectedCount), 32u);
    EXPECT_EQ(offsetof(UBOStructures::SelectionOutlineUBO, OutlineWidth), 36u);

    // Row 3+: SelectedIDs[16] (ivec4 array) — offset 48, size 256
    EXPECT_EQ(offsetof(UBOStructures::SelectionOutlineUBO, SelectedIDs), 48u);
}

TEST(SelectionOutlineUBO, DefaultOutlineColorIsOrange)
{
    UBOStructures::SelectionOutlineUBO ubo;

    EXPECT_FLOAT_EQ(ubo.OutlineColor.r, 1.0f);
    EXPECT_FLOAT_EQ(ubo.OutlineColor.g, 0.5f);
    EXPECT_FLOAT_EQ(ubo.OutlineColor.b, 0.0f);
    EXPECT_FLOAT_EQ(ubo.OutlineColor.a, 0.8f);
}

TEST(SelectionOutlineUBO, DefaultSelectedCountIsZero)
{
    UBOStructures::SelectionOutlineUBO ubo;
    EXPECT_EQ(ubo.SelectedCount, 0);
}

TEST(SelectionOutlineUBO, DefaultOutlineWidthIsOne)
{
    UBOStructures::SelectionOutlineUBO ubo;
    EXPECT_EQ(ubo.OutlineWidth, 1);
}

TEST(SelectionOutlineUBO, SelectedIDsDefaultToNegativeOneSentinel)
{
    UBOStructures::SelectionOutlineUBO ubo;

    for (int i = 0; i < 16; ++i)
    {
        EXPECT_EQ(ubo.SelectedIDs[i], glm::ivec4(-1));
    }
}

// =============================================================================
// UBO Binding Constant
// =============================================================================

TEST(SelectionOutlineUBO, BindingSlotIs27)
{
    EXPECT_EQ(ShaderBindingLayout::UBO_SELECTION_OUTLINE, 27u);
    EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_SELECTION_OUTLINE, "SelectionOutlineUBO"));
}

// =============================================================================
// RenderGraph Wiring Tests
// =============================================================================

TEST(SelectionOutlineGraph, PassInsertedBetweenPostProcessAndUIComposite)
{
    RenderGraph graph;

    auto postProcess = AddStub(graph, "PostProcessPass");
    auto selOutline = AddStub(graph, "SelectionOutlinePass");
    auto uiComposite = AddStub(graph, "UICompositePass");
    auto finalPass = AddStub(graph, "FinalPass");

    graph.ConnectPass("PostProcessPass", "SelectionOutlinePass");
    graph.ConnectPass("SelectionOutlinePass", "UICompositePass");
    graph.ConnectPass("UICompositePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    // Graph should compile and all passes should be retrievable
    auto retrievedOutline = graph.GetPass<OutlineStubPass>("SelectionOutlinePass");
    ASSERT_NE(retrievedOutline, nullptr);
    EXPECT_EQ(retrievedOutline->GetName(), "SelectionOutlinePass");

    auto retrievedPost = graph.GetPass<OutlineStubPass>("PostProcessPass");
    ASSERT_NE(retrievedPost, nullptr);

    auto retrievedUI = graph.GetPass<OutlineStubPass>("UICompositePass");
    ASSERT_NE(retrievedUI, nullptr);
}

TEST(SelectionOutlineGraph, TopologicalOrderRespectsChain)
{
    RenderGraph graph;
    graph.Init(100, 100);

    auto postProcess = AddStub(graph, "PostProcessPass");
    auto selOutline = AddStub(graph, "SelectionOutlinePass");
    auto uiComposite = AddStub(graph, "UICompositePass");
    auto finalPass = AddStub(graph, "FinalPass");

    graph.ConnectPass("PostProcessPass", "SelectionOutlinePass");
    graph.ConnectPass("SelectionOutlinePass", "UICompositePass");
    graph.ConnectPass("UICompositePass", "FinalPass");
    graph.SetFinalPass("FinalPass");

    // Execute triggers UpdateDependencyGraph and populates pass order
    graph.Execute();

    auto const& order = graph.GetPassOrder();
    ASSERT_FALSE(order.empty());

    // Find indices in topological order
    int postIdx = -1, outlineIdx = -1, uiIdx = -1, finalIdx = -1;
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
    {
        if (order[i] == "PostProcessPass")
            postIdx = i;
        else if (order[i] == "SelectionOutlinePass")
            outlineIdx = i;
        else if (order[i] == "UICompositePass")
            uiIdx = i;
        else if (order[i] == "FinalPass")
            finalIdx = i;
    }

    EXPECT_NE(postIdx, -1);
    EXPECT_NE(outlineIdx, -1);
    EXPECT_NE(uiIdx, -1);
    EXPECT_NE(finalIdx, -1);

    // Verify ordering: PostProcess < SelectionOutline < UIComposite < Final
    EXPECT_LT(postIdx, outlineIdx);
    EXPECT_LT(outlineIdx, uiIdx);
    EXPECT_LT(uiIdx, finalIdx);
}

// =============================================================================
// Entity ID Packing Tests
// =============================================================================

TEST(SelectionOutlineUBO, PackSingleEntityID)
{
    UBOStructures::SelectionOutlineUBO ubo;
    ubo.SelectedCount = 1;
    ubo.SelectedIDs[0] = glm::ivec4(42, -1, -1, -1);

    EXPECT_EQ(ubo.SelectedIDs[0].x, 42);
    EXPECT_EQ(ubo.SelectedCount, 1);
}

TEST(SelectionOutlineUBO, Pack64EntityIDs)
{
    UBOStructures::SelectionOutlineUBO ubo;

    // Fill all 64 slots
    for (int i = 0; i < 64; ++i)
    {
        int vecIndex = i / 4;
        int compIndex = i % 4;
        ubo.SelectedIDs[vecIndex][compIndex] = i + 100;
    }
    ubo.SelectedCount = 64;

    // Verify all 64 IDs are stored correctly
    for (int i = 0; i < 64; ++i)
    {
        int vecIndex = i / 4;
        int compIndex = i % 4;
        EXPECT_EQ(ubo.SelectedIDs[vecIndex][compIndex], i + 100);
    }
}
