#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure reasoning behind olo_render_why_not_visible (issue #306
// item A, rendering half; the inspection counterpart called out by #316 Part 3).
// The reasoning lives in a header-only free function with no EnTT / renderer /
// editor / GPU dependencies precisely so it can be exercised here without a live
// editor or GPU — the test binary compiles the MCP dispatch core but deliberately
// NOT McpTools.cpp (the editor-backed handler). The live tool is verified
// separately over the MCP attach loop; this pins the verdict cascade that maps
// gathered facts -> root-cause explanation.
#include "MCP/McpRenderExplain.h"

#include <algorithm>
#include <string>

namespace
{
    using OloEngine::MCP::RenderExplain::EntityRenderFacts;
    using OloEngine::MCP::RenderExplain::ExplainWhyNotVisible;
    using OloEngine::MCP::RenderExplain::WhyNotVisibleInput;
    using OloEngine::MCP::RenderExplain::WhyNotVisibleVerdict;

    // The canonical "this entity is fully set up to render AND is on screen" input.
    // A mesh with geometry, non-degenerate scale, clean shader, in front of the
    // camera and inside the frustum. Each test mutates exactly one field to isolate
    // a single branch of the cascade.
    WhyNotVisibleInput MakeVisible()
    {
        WhyNotVisibleInput in;
        in.SceneLoaded = true;
        in.CameraKnown = true;
        in.AnyShaderHasErrors = false;
        in.ShaderErrorCount = 0;

        EntityRenderFacts& e = in.Entity;
        e.EntityExists = true;
        e.HasRenderable = true;
        e.RenderableKind = "MeshComponent";
        e.GeometryRequired = true;
        e.GeometryPresent = true;
        e.HasVisibilityFlag = false;
        e.VisibilityFlagOn = true;
        e.ScaleDegenerate = false;
        e.HasMaterialShader = true;
        e.MaterialShaderName = "PBR";
        e.MaterialShaderHasErrors = false;
        e.BoundsKnown = true;
        e.BehindCamera = false;
        e.InFrustum = true;
        return in;
    }

    bool HasCheckContaining(const WhyNotVisibleVerdict& v, const std::string& needle)
    {
        return std::any_of(v.Checks.begin(), v.Checks.end(),
                           [&](const std::string& line)
                           { return line.find(needle) != std::string::npos; });
    }
} // namespace

TEST(McpRenderExplain, FullyConfiguredAndInViewShouldBeVisible)
{
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(MakeVisible());
    EXPECT_EQ("should_be_visible", v.ReasonCode);
    EXPECT_TRUE(v.RenderableConfigOk);
    EXPECT_TRUE(v.Visible);
    EXPECT_FALSE(v.Summary.empty());
    // No failing checks on the happy path.
    EXPECT_FALSE(HasCheckContaining(v, "[fail]"));
    // The frustum gate should have been recorded as passed.
    EXPECT_TRUE(HasCheckContaining(v, "[ok] the entity is inside the editor camera frustum"));
}

TEST(McpRenderExplain, NoSceneIsRejectedFirst)
{
    WhyNotVisibleInput in = MakeVisible();
    in.SceneLoaded = false;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("no_scene", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
    EXPECT_FALSE(v.Visible);
    // Rejected before any later checks were recorded.
    EXPECT_EQ(1u, v.Checks.size());
}

TEST(McpRenderExplain, EntityMissing)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.EntityExists = false;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("entity_missing", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
}

TEST(McpRenderExplain, NotRenderable)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.HasRenderable = false;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("not_renderable", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
}

TEST(McpRenderExplain, GeometryMissingReportedAfterRenderable)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.GeometryPresent = false;
    in.Entity.GeometryDetail = "the MeshComponent's MeshSource is null";
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("geometry_missing", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
    EXPECT_TRUE(HasCheckContaining(v, "[ok] the entity has a renderable component (MeshComponent)"));
    // The handler-provided specifics are woven into the summary.
    EXPECT_NE(std::string::npos, v.Summary.find("MeshSource is null"));
}

TEST(McpRenderExplain, GeometryNotRequiredSkipsTheGeometryGate)
{
    // A sprite/circle/text renderable needs no asset — a "missing" asset must NOT
    // trip geometry_missing.
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.RenderableKind = "SpriteRendererComponent";
    in.Entity.GeometryRequired = false;
    in.Entity.GeometryPresent = false; // ignored when GeometryRequired is false
    in.Entity.BoundsKnown = false;     // 2D renderables skip the camera checks
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_NE("geometry_missing", v.ReasonCode);
    EXPECT_EQ("should_be_visible", v.ReasonCode);
    EXPECT_TRUE(v.RenderableConfigOk);
}

TEST(McpRenderExplain, ComponentHidden)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.RenderableKind = "ModelComponent";
    in.Entity.HasVisibilityFlag = true;
    in.Entity.VisibilityFlagOn = false;
    in.Entity.VisibilityFlagName = "ModelComponent.m_Visible";
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("component_hidden", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
    EXPECT_NE(std::string::npos, v.Summary.find("ModelComponent.m_Visible"));
}

TEST(McpRenderExplain, DegenerateScale)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.ScaleDegenerate = true;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("degenerate_scale", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
}

TEST(McpRenderExplain, ShaderCompileError)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.HasMaterialShader = true;
    in.Entity.MaterialShaderName = "CustomGlow";
    in.Entity.MaterialShaderHasErrors = true;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("shader_compile_error", v.ReasonCode);
    EXPECT_FALSE(v.RenderableConfigOk);
    // The offending shader name should appear so the user can olo_shader_get it.
    EXPECT_NE(std::string::npos, v.Summary.find("CustomGlow"));
}

TEST(McpRenderExplain, UnresolvedMaterialShaderSkipsTheShaderGate)
{
    // Standard meshes carry no own shader (the shared PBR shader is used). A
    // missing material shader must NOT block — and global errors are only a hint.
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.HasMaterialShader = false;
    in.Entity.MaterialShaderHasErrors = true; // ignored when HasMaterialShader is false
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_NE("shader_compile_error", v.ReasonCode);
    EXPECT_EQ("should_be_visible", v.ReasonCode);
}

TEST(McpRenderExplain, BehindCamera)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.BehindCamera = true;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("behind_camera", v.ReasonCode);
    // The object is configured fine — it is just off-screen.
    EXPECT_TRUE(v.RenderableConfigOk);
    EXPECT_FALSE(v.Visible);
}

TEST(McpRenderExplain, OutsideFrustumReportedAfterBehindCamera)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.BehindCamera = false;
    in.Entity.InFrustum = false;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("outside_frustum", v.ReasonCode);
    EXPECT_TRUE(v.RenderableConfigOk);
    EXPECT_FALSE(v.Visible);
    EXPECT_TRUE(HasCheckContaining(v, "[ok] the entity is in front of the editor camera"));
}

TEST(McpRenderExplain, CameraChecksSkippedWhenCameraUnknown)
{
    // Config is fine but no camera pose: report should_be_visible WITHOUT asserting
    // it is on screen (Visible stays false), and note the skipped camera checks.
    WhyNotVisibleInput in = MakeVisible();
    in.CameraKnown = false;
    in.Entity.BehindCamera = true; // must be ignored — camera checks can't run
    in.Entity.InFrustum = false;   // must be ignored
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("should_be_visible", v.ReasonCode);
    EXPECT_TRUE(v.RenderableConfigOk);
    EXPECT_FALSE(v.Visible);
    EXPECT_TRUE(HasCheckContaining(v, "camera-relative checks skipped"));
}

TEST(McpRenderExplain, CameraChecksSkippedWhenBoundsUnknown)
{
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.BoundsKnown = false;
    in.Entity.BehindCamera = true; // must be ignored — no bounds to test
    in.Entity.InFrustum = false;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("should_be_visible", v.ReasonCode);
    EXPECT_FALSE(v.Visible);
    EXPECT_TRUE(HasCheckContaining(v, "camera-relative checks skipped"));
}

TEST(McpRenderExplain, GlobalShaderErrorsAreAHintNotABlocker)
{
    // An otherwise-visible entity with global shader errors still reports
    // should_be_visible, but the summary/checks surface the broken shaders.
    WhyNotVisibleInput in = MakeVisible();
    in.AnyShaderHasErrors = true;
    in.ShaderErrorCount = 3;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("should_be_visible", v.ReasonCode);
    EXPECT_TRUE(HasCheckContaining(v, "shader(s) currently fail to compile"));
    EXPECT_NE(std::string::npos, v.Summary.find("3 shader(s)"));
}

TEST(McpRenderExplain, ConfigGatesPrecedeCameraGates)
{
    // Both a config problem (degenerate scale) and a camera problem (behind
    // camera) present: the root cause must be the config gate, not the symptom.
    WhyNotVisibleInput in = MakeVisible();
    in.Entity.ScaleDegenerate = true;
    in.Entity.BehindCamera = true;
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(in);
    EXPECT_EQ("degenerate_scale", v.ReasonCode);
}

TEST(McpRenderExplain, ChecksAreOrderedAndPrefixed)
{
    const WhyNotVisibleVerdict v = ExplainWhyNotVisible(MakeVisible());
    ASSERT_FALSE(v.Checks.empty());
    // First recorded check is always the scene-loaded gate.
    EXPECT_NE(std::string::npos, v.Checks.front().find("active scene"));
    // Every line carries one of the status prefixes.
    for (const std::string& line : v.Checks)
    {
        const bool prefixed = line.rfind("[ok] ", 0) == 0 || line.rfind("[fail] ", 0) == 0 || line.rfind("[warn] ", 0) == 0;
        EXPECT_TRUE(prefixed) << "unprefixed check line: " << line;
    }
}
