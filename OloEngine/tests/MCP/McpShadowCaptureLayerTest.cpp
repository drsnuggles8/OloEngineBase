// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the array-layer selection behind olo_render_capture_target's
// 'layer' argument (issue #607) — the piece that makes the CSM cascade array and
// the shadow atlas (and their comparison-OFF raw-depth views, ShadowCSMRaw /
// ShadowAtlasRaw) capturable one cascade at a time.
//
// Both behaviours pinned here are anti-"confidently wrong answer" rules:
//
//   * A per-cascade layer VIEW resolves to its PARENT texture object (that is
//     what a sampler binding wants), so a readback that ignores the view's own
//     layer reads layer 0 — capturing "ShadowMapCSMCascade3" would hand back
//     cascade 0's pixels with no error whatsoever. The view's layer must be the
//     DEFAULT.
//   * An out-of-range layer must be an ERROR. Clamping cascade 7 to cascade 3 —
//     or reading layer 0 of a non-array target as if it were the requested
//     layer — is exactly the silent-wrong-answer class these diagnostics exist
//     to remove.
#include "MCP/McpShadowCapture.h"

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::MCP::CaptureLayer;

    // The CSM cascade array as the render graph describes it: 4 layers, not a view.
    TargetLayers CsmArray()
    {
        return TargetLayers{ .LayerCount = 4, .ViewLayer = 0 };
    }

    // "ShadowMapCSMCascade3": a single-layer VIEW of layer 3 of that array.
    TargetLayers CascadeView(u32 layer)
    {
        return TargetLayers{ .LayerCount = 1, .ViewLayer = layer };
    }

    // A plain 2D target (SceneColor).
    TargetLayers Plain2D()
    {
        return TargetLayers{ .LayerCount = 1, .ViewLayer = 0 };
    }

    TEST(McpShadowCaptureLayer, ArrayTargetDefaultsToLayerZeroAndSaysSo)
    {
        const Selection s = SelectLayer(CsmArray(), "ShadowMapCSM", /*hasRequested*/ false, 0);
        EXPECT_TRUE(s.Error.empty());
        EXPECT_EQ(s.Layer, 0u);
        // The note is what stops an agent reading "the CSM" and believing it saw
        // all four cascades.
        EXPECT_NE(s.Note.find("4-layer array"), std::string::npos);
    }

    TEST(McpShadowCaptureLayer, ArrayTargetHonoursEveryValidLayer)
    {
        for (u32 layer = 0; layer < 4; ++layer)
        {
            const Selection s = SelectLayer(CsmArray(), "ShadowMapCSM", true, layer);
            EXPECT_TRUE(s.Error.empty()) << s.Error;
            EXPECT_EQ(s.Layer, layer);
        }
    }

    TEST(McpShadowCaptureLayer, OutOfRangeLayerIsAnErrorNotAClamp)
    {
        const Selection s = SelectLayer(CsmArray(), "ShadowMapCSM", true, 7);
        ASSERT_FALSE(s.Error.empty());
        EXPECT_NE(s.Error.find("out of range"), std::string::npos);
        EXPECT_NE(s.Error.find("0..3"), std::string::npos);
    }

    TEST(McpShadowCaptureLayer, NegativeLayerIsAnError)
    {
        const Selection s = SelectLayer(CsmArray(), "ShadowMapCSM", true, -1);
        EXPECT_FALSE(s.Error.empty());
    }

    // The bug this whole core exists for.
    TEST(McpShadowCaptureLayer, LayerViewDefaultsToItsOwnLayerNotLayerZero)
    {
        const Selection s = SelectLayer(CascadeView(3), "ShadowMapCSMCascade3", /*hasRequested*/ false, 0);
        EXPECT_TRUE(s.Error.empty());
        EXPECT_EQ(s.Layer, 3u) << "capturing a cascade view must read THAT cascade, not layer 0";
        EXPECT_FALSE(s.Note.empty());
    }

    TEST(McpShadowCaptureLayer, LayerViewExposesExactlyOneLayer)
    {
        // Index 0 of the view IS the view's layer.
        const Selection zero = SelectLayer(CascadeView(2), "ShadowMapCSMCascade2", true, 0);
        EXPECT_TRUE(zero.Error.empty());
        EXPECT_EQ(zero.Layer, 2u);

        // Anything else does not exist through that name — and must not silently
        // reach a sibling cascade.
        const Selection other = SelectLayer(CascadeView(2), "ShadowMapCSMCascade2", true, 1);
        EXPECT_FALSE(other.Error.empty());
    }

    TEST(McpShadowCaptureLayer, LayerOnANonArrayTargetIsAnError)
    {
        const Selection s = SelectLayer(Plain2D(), "SceneColor", true, 2);
        ASSERT_FALSE(s.Error.empty());
        EXPECT_NE(s.Error.find("not a texture array"), std::string::npos);

        // ...but layer 0 on a plain 2D target is the normal, quiet case.
        const Selection zero = SelectLayer(Plain2D(), "SceneColor", true, 0);
        EXPECT_TRUE(zero.Error.empty());
        EXPECT_EQ(zero.Layer, 0u);
        EXPECT_TRUE(zero.Note.empty());
    }

    TEST(McpShadowCaptureLayer, IsArrayTargetDistinguishesTheAtlasFromTheCascadeArray)
    {
        // The shadow atlas is a 1-layer array: every spot/point-face tile is a
        // sub-RECT of layer 0, so there is no layer to pick — reporting it as an
        // array would send an agent hunting for cascades that do not exist.
        EXPECT_FALSE(IsArrayTarget(TargetLayers{ .LayerCount = 1, .ViewLayer = 0 }));
        EXPECT_TRUE(IsArrayTarget(CsmArray()));
    }
} // namespace
