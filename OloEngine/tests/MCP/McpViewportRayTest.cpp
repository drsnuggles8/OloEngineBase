#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpViewportRayTest — unit test (headless, no GL, no live editor).
//
// Pins MCP/McpViewportRay.h, the viewport ray source shared by olo_terrain_pick
// and olo_rt_trace_ray (#607), and through it EditorLayer::BuildMouseRay, which
// delegates its arithmetic here.
//
// The invariant this file exists for: a viewport coordinate is TOP-LEFT origin
// on every backend, and the camera matrix the CPU holds is GL-shaped on every
// backend, so the TOP of the viewport unprojects ABOVE the camera's forward axis
// with no backend branch. Before #607 the brush ray read a row-flipped y that
// matched only GL, so the ray was mirrored on Vulkan; MCP terrain pick passed the
// unflipped y straight in, so it was mirrored on GL.
// =============================================================================

#include "MCP/McpViewportRay.h"

#include <glm/gtc/matrix_transform.hpp>

#include <limits>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    namespace VR = OloEngine::MCP::ViewportRay;
    using Json = nlohmann::json;

    // Camera at +Z looking down -Z, +Y up — the editor's default orientation.
    [[nodiscard]] glm::mat4 CameraViewProjection(f32 aspect = 16.0f / 9.0f)
    {
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        return projection * view;
    }

    void ExpectNormalized(const VR::ViewportInput& input, f32 x, f32 y)
    {
        const glm::vec2 n = VR::Normalized(input);
        EXPECT_FLOAT_EQ(n.x, x);
        EXPECT_FLOAT_EQ(n.y, y);
    }

    [[nodiscard]] VR::CameraRay Unproject(const glm::vec2& normalized, f32 aspect = 16.0f / 9.0f)
    {
        const auto ray = VR::UnprojectNormalized(normalized, CameraViewProjection(aspect));
        EXPECT_TRUE(ray.has_value()) << "at " << normalized.x << ", " << normalized.y;
        return ray.value_or(VR::CameraRay{});
    }
} // namespace

TEST(McpViewportRay, TheViewportCentreLooksDownTheCameraAxis)
{
    const VR::CameraRay ray = Unproject({ 0.5f, 0.5f });
    EXPECT_NEAR(ray.Direction.x, 0.0f, 1e-5f);
    EXPECT_NEAR(ray.Direction.y, 0.0f, 1e-5f);
    EXPECT_NEAR(ray.Direction.z, -1.0f, 1e-5f);
    // Origin on the near plane (camera z 10, near 0.1), span to the far plane.
    EXPECT_NEAR(ray.Origin.z, 9.9f, 1e-4f);
    EXPECT_NEAR(ray.Length, 99.9f, 1e-2f);
}

TEST(McpViewportRay, TopOfTheViewportIsUpAndLeftIsLeftWithNoBackendBranch)
{
    // The mirrored-ray bug in one assertion: y = 0 is the TOP row, which is
    // above the camera axis. A bottom-up reading would put it below.
    EXPECT_GT(Unproject({ 0.5f, 0.0f }).Direction.y, 0.1f);
    EXPECT_LT(Unproject({ 0.5f, 1.0f }).Direction.y, -0.1f);
    EXPECT_LT(Unproject({ 0.0f, 0.5f }).Direction.x, -0.1f);
    EXPECT_GT(Unproject({ 1.0f, 0.5f }).Direction.x, 0.1f);

    // Symmetric about the centre: the top and bottom edges are mirror images.
    const VR::CameraRay top = Unproject({ 0.25f, 0.0f });
    const VR::CameraRay bottom = Unproject({ 0.25f, 1.0f });
    EXPECT_NEAR(top.Direction.y, -bottom.Direction.y, 1e-5f);
    EXPECT_NEAR(top.Direction.x, bottom.Direction.x, 1e-5f);
}

TEST(McpViewportRay, APixelResolvesThroughTheSizeTheCallerMeasuredItIn)
{
    // A pixel off a native 1280x720 capture and the same point off the 640x360
    // downscale olo_screenshot hands back must be the same ray: the render
    // resolution (which a non-native upscale shrinks) never enters.
    Json native{ { "viewportPixel", { { "coordinate", { 960.0, 180.0 } }, { "width", 1280 }, { "height", 720 } } } };
    Json downscaled{ { "viewportPixel", { { "coordinate", { 480.0, 90.0 } }, { "width", 640 }, { "height", 360 } } } };
    VR::ViewportInput a;
    VR::ViewportInput b;
    ASSERT_FALSE(VR::ParseViewportInput(native, a).has_value());
    ASSERT_FALSE(VR::ParseViewportInput(downscaled, b).has_value());
    ExpectNormalized(a, 0.75f, 0.25f);
    ExpectNormalized(b, 0.75f, 0.25f);

    VR::ViewportInput normalized;
    ASSERT_FALSE(VR::ParseViewportInput(Json{ { "viewportNormalized", { 0.75, 0.25 } } }, normalized).has_value());
    ExpectNormalized(normalized, 0.75f, 0.25f);

    const Json echoed = VR::ViewportInputJson(a);
    EXPECT_EQ(echoed["source"], "viewportPixel");
    EXPECT_EQ(echoed["viewport"]["width"], 1280u);
    EXPECT_FLOAT_EQ(echoed["normalized"][0].get<f32>(), 0.75f);
    EXPECT_FLOAT_EQ(echoed["normalized"][1].get<f32>(), 0.25f);
}

TEST(McpViewportRay, ExactlyOneSourceIsRequired)
{
    const Json pixel{ { "coordinate", { 1.0, 1.0 } }, { "width", 4 }, { "height", 4 } };
    EXPECT_FALSE(VR::CheckExactlyOneSource(Json{ { "viewportPixel", pixel } }, "rays").has_value());
    EXPECT_FALSE(VR::CheckExactlyOneSource(Json{ { "viewportNormalized", { 0.5, 0.5 } } }, "rays").has_value());
    EXPECT_FALSE(VR::CheckExactlyOneSource(Json{ { "rays", Json::array() } }, "rays").has_value());
    // A null member counts as absent, as it does everywhere else in the tool surface.
    EXPECT_FALSE(VR::CheckExactlyOneSource(Json{ { "rays", Json::array() }, { "viewportPixel", nullptr } }, "rays")
                     .has_value());

    EXPECT_TRUE(VR::CheckExactlyOneSource(Json::object(), "rays").has_value());
    const auto mixed = VR::CheckExactlyOneSource(Json{ { "rays", Json::array() }, { "viewportNormalized", { 0.5, 0.5 } } }, "rays");
    ASSERT_TRUE(mixed.has_value());
    EXPECT_NE(mixed->find("exactly one"), std::string::npos);
    EXPECT_NE(mixed->find("'rays'"), std::string::npos) << "the tool's own world-form key is named";
}

TEST(McpViewportRay, OutOfRangeAndNonFiniteCoordinatesAreRefused)
{
    VR::ViewportInput input;
    const auto pixel = [](f64 x, f64 y, i64 w, i64 h)
    { return Json{ { "viewportPixel", { { "coordinate", { x, y } }, { "width", w }, { "height", h } } } }; };
    EXPECT_TRUE(VR::ParseViewportInput(pixel(-0.5, 1.0, 8, 8), input).has_value());
    EXPECT_TRUE(VR::ParseViewportInput(pixel(8.0, 1.0, 8, 8), input).has_value()) << "x == width is outside";
    EXPECT_TRUE(VR::ParseViewportInput(pixel(1.0, 1.0, 0, 8), input).has_value());
    EXPECT_TRUE(VR::ParseViewportInput(pixel(1.0, 1.0, 8, -8), input).has_value());
    EXPECT_TRUE(VR::ParseViewportInput(pixel(std::numeric_limits<f64>::quiet_NaN(), 1.0, 8, 8), input).has_value());
    EXPECT_TRUE(VR::ParseViewportInput(Json{ { "viewportNormalized", { 1.01, 0.5 } } }, input).has_value());
    EXPECT_TRUE(VR::ParseViewportInput(Json{ { "viewportNormalized", { 0.5 } } }, input).has_value());
    EXPECT_FALSE(VR::ParseViewportInput(Json{ { "viewportNormalized", { 0.0, 1.0 } } }, input).has_value());
}

TEST(McpViewportRay, ADegenerateCameraGivesNoRayRatherThanNaNs)
{
    EXPECT_FALSE(VR::UnprojectNormalized({ 0.5f, 0.5f }, glm::mat4(0.0f)).has_value());
    glm::mat4 nan(1.0f);
    nan[0][0] = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(VR::UnprojectNormalized({ 0.5f, 0.5f }, nan).has_value());
}
