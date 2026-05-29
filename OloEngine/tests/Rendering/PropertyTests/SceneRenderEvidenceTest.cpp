// =============================================================================
// SceneRenderEvidenceTest.cpp
//
// Issue #258: a visual-evidence test that renders through the REAL Scene
// pipeline — `Scene::OnUpdateRuntime` -> `RenderScene3D` -> the full
// Renderer3D render graph (shadow, scene, post-process, tone-map, UI
// composite) — and reads the final composited frame back for pixel
// assertions. Unlike the L8 `SphereAreaLightVisualTest` (which drives a
// single fullscreen probe shader directly), this exercises the same path
// the editor viewport uses, end-to-end, from inside a test.
//
// This is the capability issue #258 was about: before the
// `RendererAttachedTest` fixture gained a sized render path + GL-state
// hygiene + a process-wide renderer-shutdown environment, a full-pipeline
// Scene render in a test left global GL state corrupted for later tests and
// SIGSEGV'd at process teardown, so screenshot/visual tests had to stay
// `DISABLED_`. With those fixed, this test runs in the normal suite (and in
// CI when a GL 4.6 context is available; it SKIPs otherwise).
//
// Scene: a primary camera, a point light off-axis, and a single unit cube
// (MeshPrimitives::CreateCube) with the default PBR material, rotated so
// several differently-lit faces are visible. Contracts asserted (all
// driver-independent):
//
//   1. The composited frame reads back at the requested resolution.
//   2. The frame is non-trivial: it is not a single flat colour — the lit
//      cube against the background produces real luminance contrast.
//   3. The cube is actually drawn at screen centre: the centre region is
//      measurably different from the corner (background) region.
//
// The readback PNG is always written to
//   OloEditor/assets/tests/visual/SceneRender_VisualEvidence.png
// next to the other visual baselines, so a reviewer always has the frame.
//
// Classification: L8 / integration (full GL pipeline through the real Scene
// render path, RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 256;

        f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        // Mean luminance over a square region centred at (cx, cy) with the
        // given half-extent, clamped to the image bounds.
        f32 MeanLuminanceInRegion(const std::vector<u8>& px, u32 w, u32 h,
                                  u32 cx, u32 cy, u32 halfExtent)
        {
            const u32 x0 = (cx > halfExtent) ? cx - halfExtent : 0u;
            const u32 y0 = (cy > halfExtent) ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent, w);
            const u32 y1 = std::min(cy + halfExtent, h);
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    sum += LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4);
                    ++count;
                }
            }
            return count ? static_cast<f32>(sum / count) : 0.0f;
        }

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "SceneRender_VisualEvidence.png";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // LitCubeScene — a minimal renderable scene driven through OnUpdateRuntime.
    // -------------------------------------------------------------------------
    class LitCubeScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Camera at +Z looking toward the origin (identity rotation =>
            // looks down -Z in OloEngine's convention). SceneCamera defaults
            // to ORTHOGRAPHIC with a [-1, 1] depth range, which would clip a
            // cube 3.5 units away to nothing — force perspective so the cube
            // is actually in view.
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.5f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // Off-axis point light so the cube's faces are differently lit
            // (gives the centre-vs-corner and contrast contracts something to
            // measure regardless of the default ambient term).
            Entity light = GetScene().CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 2.5f, 2.5f, 3.0f };
            auto& pointLight = light.AddComponent<PointLightComponent>();
            pointLight.m_Color = { 1.0f, 1.0f, 1.0f };
            pointLight.m_Intensity = 40.0f;
            pointLight.m_Range = 30.0f;

            // Unit cube at the origin, rotated so three faces are visible.
            // No MaterialComponent -> the render path uses the default PBR
            // material.
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            Entity cubeEntity = GetScene().CreateEntity("Cube");
            cubeEntity.AddComponent<MeshComponent>(cube->GetMeshSource());
            cubeEntity.GetComponent<TransformComponent>().SetRotationEuler({ 0.45f, 0.8f, 0.0f });

            EnableRendering(kSize, kSize);
        }
    };

    TEST_F(LitCubeScene, RendersThroughScenePipelineAndProducesPng)
    {
        // Two frames: the render graph seeds prev-frame history (TAA /
        // velocity) on the first and produces the stable image on the second.
        RunFrames(2);

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        EXPECT_EQ(width, kSize);
        EXPECT_EQ(height, kSize);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

        // Always write the PNG first, then assert — keeps the artifact
        // available even when a later assertion fails.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(width), static_cast<int>(height),
                                           4, px.data(), static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        // Contrast contract: a single flat colour (clear-only, no geometry, or
        // a black frame) has near-zero luminance spread. A lit cube against
        // the background must produce real contrast.
        f32 minLum = 1.0f;
        f32 maxLum = 0.0f;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const f32 l = LuminanceAt(px, i);
            minLum = std::min(minLum, l);
            maxLum = std::max(maxLum, l);
        }
        EXPECT_GT(maxLum - minLum, 0.05f)
            << "rendered frame is nearly flat (min=" << minLum << ", max=" << maxLum
            << ") — the cube may not have drawn; see " << out.string();
        EXPECT_GT(maxLum, 0.10f)
            << "rendered frame has no bright pixels — pipeline may have output black; see "
            << out.string();

        // Geometry-at-centre contract: the cube sits at screen centre, so the
        // centre region must differ from the corner (background) region.
        const f32 centre = MeanLuminanceInRegion(px, width, height, width / 2, height / 2, 24);
        const f32 corner = MeanLuminanceInRegion(px, width, height, 12, 12, 12);
        EXPECT_GT(std::abs(centre - corner), 0.02f)
            << "screen-centre region (" << centre << ") does not differ from the corner/background ("
            << corner << ") — the cube may not be visible; see " << out.string();
    }
} // namespace OloEngine::Tests
