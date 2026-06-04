// =============================================================================
// SSRVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the general
// Screen-Space Reflections pass (PostProcess_SSR.glsl / SSRRenderPass).
//
// A near-mirror metallic floor sits under a bright RED emissive block. The
// block's reflection should appear in the floor between the camera and the
// block. The scene is rendered twice through the FULL deferred Renderer3D
// pipeline from the same pose — once with SSR OFF and once with SSR ON — and
// both frames are written to
//   OloEditor/assets/tests/visual/SSR_<state>.png
//
// The contract is GOLDEN-FREE and differential, so it is robust across GPUs and
// needs no committed reference image: turning SSR on must add the block's red
// into the floor region (the floor patch gets measurably redder), and that red
// must be the dominant channel there (a reflection of the red block, not a grey
// wash). The cheap SSR *math* contracts (projection round-trip, Fresnel, fade
// curves, octahedral decode) live in ScreenSpaceReflectionMathTest.cpp.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching WaterVisualEvidenceTest. SSR is deferred-only, so the fixture forces
// the deferred render path.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr f32 kCaptureTime = 4.0f; // freeze the clock for deterministic frames

        struct BandStats
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;
        };

        // Mean RGB over a rectangular band (UV fractions), rows top-down.
        [[nodiscard]] BandStats SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sumR += px[idx + 0];
                    sumG += px[idx + 1];
                    sumB += px[idx + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / count, static_cast<f64>(sumG) / count,
                     static_cast<f64>(sumB) / count };
        }
    } // namespace

    class SSRVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // SSR only runs on the deferred path (it reads the G-Buffer).
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Sun.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 2.5f;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                return e;
            };

            // Near-mirror metallic floor at y = 0. White albedo + metallic = 1 +
            // very low roughness makes it a strong neutral mirror so the red
            // block's reflection comes through with its own colour.
            {
                Entity floor = addMesh("MirrorFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 60.0f, 1.0f, 60.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));
                mat.m_Material.SetMetallicFactor(1.0f);
                mat.m_Material.SetRoughnessFactor(0.04f);
            }

            // Bright RED emissive block hovering above the floor — the thing whose
            // reflection we look for. Emissive so it is unambiguously red in the
            // lit scene colour that SSR samples.
            {
                Entity block = addMesh("RedBlock", MeshPrimitive::Cube, { 0.0f, 3.0f, -7.0f },
                                       { 4.0f, 2.0f, 1.0f });
                auto& mat = block.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(3.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        // Render the current scene/settings from a fixed pose and read back the
        // composited frame (top-down rows), also saving it as PNG evidence.
        void Capture(const std::string& state, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Above the floor, looking forward and down at it; the red block sits
            // ahead at y=3, z=-7, so its reflection lands in the floor between the
            // camera and the block.
            camera.SetPose(glm::vec3(0.0f, 4.0f, 6.0f), 0.0f, 0.42f);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for SSR state '" << state << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame.
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("SSR_" + state + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             outPixels.data(), static_cast<int>(kWidth) * 4);
        }
    };

    // SSR off vs on: the floor must gain the red block's reflection. SKIPs without
    // a GL 4.6 context (see the file header).
    TEST_F(SSRVisualEvidenceTest, ReflectionAppearsOnMirrorFloor)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t) { Time::SetMockTime(t); }
            ~ScopedMockTime() { Time::ClearMockTime(); }
        } scopedMockTime(kCaptureTime);

        // Floor band: lower-centre of the frame, below the block itself, where the
        // reflection is expected. Generous so the contract does not hinge on the
        // exact pixel the reflection lands on.
        constexpr f32 bx0 = 0.30f, bx1 = 0.70f, by0 = 0.58f, by1 = 0.88f;

        auto& pp = Renderer3D::GetPostProcessSettings();

        // --- SSR OFF baseline ---
        pp.SSREnabled = false;
        std::vector<u8> offPixels;
        Capture("Off", offPixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const BandStats off = SampleBand(offPixels, bx0, bx1, by0, by1);

        // --- SSR ON ---
        pp.SSREnabled = true;
        pp.SSRIntensity = 1.0f;
        pp.SSRMaxRoughness = 0.6f;
        pp.SSRMaxDistance = 60.0f;
        pp.SSRThickness = 1.0f;
        pp.SSRStride = 0.25f;
        pp.SSRMaxSteps = 96;
        pp.SSRBinarySearchSteps = 6;
        pp.SSREdgeFade = 0.1f;
        std::vector<u8> onPixels;
        Capture("On", onPixels);
        if (::testing::Test::HasFatalFailure())
            return;
        const BandStats on = SampleBand(onPixels, bx0, bx1, by0, by1);

        // Both frames must be non-trivial (catch a black / failed render).
        EXPECT_GT(off.R + off.G + off.B, 5.0) << "SSR-off frame rendered (near-)black";
        EXPECT_GT(on.R + on.G + on.B, 5.0) << "SSR-on frame rendered (near-)black";

        // Core contract: SSR adds the red block's reflection into the floor band.
        EXPECT_GT(on.R, off.R + 6.0)
            << "Enabling SSR did not add reflected red to the floor (off.R=" << off.R
            << " on.R=" << on.R << "). See SSR_Off.png / SSR_On.png";

        // The added reflection reads as red (the block), not a grey wash: red is
        // the dominant channel in the SSR-on floor band.
        EXPECT_GT(on.R, on.G + 4.0)
            << "SSR floor reflection is not red-dominant (R=" << on.R << " G=" << on.G
            << " B=" << on.B << "). See SSR_On.png";
        EXPECT_GT(on.R, on.B + 4.0)
            << "SSR floor reflection is not red-dominant (R=" << on.R << " G=" << on.G
            << " B=" << on.B << "). See SSR_On.png";
    }
} // namespace OloEngine::Tests
