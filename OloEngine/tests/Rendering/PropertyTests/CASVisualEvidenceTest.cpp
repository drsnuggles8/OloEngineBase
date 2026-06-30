// OLO_TEST_LAYER: L8
// =============================================================================
// CASVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for the Contrast
// Adaptive Sharpening pass (PostProcess_CAS.glsl / UpscalerRenderPass).
//
// A busy scene (a grid of bright/dark cubes plus smooth-shaded spheres on a
// grey floor, lit by two directional lights) is rendered twice through the FULL
// Renderer3D pipeline from the same pose — once with CAS OFF and once ON — and
// both composited frames are written to
//   OloEditor/assets/tests/visual/CAS_<state>.png
//
// The contract is GOLDEN-FREE and differential, so it is robust across GPUs and
// needs no committed reference image. Sharpening is, by definition, an increase
// in high-frequency content: enabling CAS must raise the frame's total gradient
// energy (sum of |luma| differences between adjacent pixels) while leaving the
// overall brightness essentially unchanged (CAS is a contrast boost at edges,
// not a global gain/level shift) and producing a frame that actually differs
// from the unsharpened one. The cheap CAS *math* contracts (flat-field identity,
// peak/valley overshoot, exact kernel value, clamp, sanitizer) live in
// CASMathTest.cpp.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching the other *VisualEvidenceTest fixtures.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
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
#include <stb_image/stb_image.h>
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
        constexpr f32 kCaptureTime = 2.0f; // freeze the clock for deterministic frames

        [[nodiscard]] f64 Luma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            return 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
        }

        // Total gradient energy: sum of absolute luma differences between
        // horizontally and vertically adjacent pixels. This is the canonical
        // "how much high-frequency detail is in the image" measure — sharpening
        // an image must raise it.
        [[nodiscard]] f64 GradientEnergy(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y + 1u < kHeight; ++y)
            {
                for (u32 x = 0; x + 1u < kWidth; ++x)
                {
                    const f64 c = Luma(px, x, y);
                    sum += std::abs(Luma(px, x + 1u, y) - c);
                    sum += std::abs(Luma(px, x, y + 1u) - c);
                }
            }
            return sum;
        }

        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += Luma(px, x, y);
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }

        // Mean absolute per-pixel luma difference between two frames (0..255).
        [[nodiscard]] f64 MeanAbsDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            f64 sum = 0.0;
            for (u32 y = 0; y < kHeight; ++y)
                for (u32 x = 0; x < kWidth; ++x)
                    sum += std::abs(Luma(a, x, y) - Luma(b, x, y));
            return sum / (static_cast<f64>(kWidth) * kHeight);
        }
    } // namespace

    class CASVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Two directional lights from opposite sides so every object carries
            // a smooth shading gradient (the soft signal CAS sharpens), plus
            // grazing rim contrast.
            {
                Entity key = scene.CreateEntity("Key");
                key.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = key.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.5f, -0.7f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 2.0f;
                dl.m_CastShadows = false;
            }
            {
                Entity fill = scene.CreateEntity("Fill");
                fill.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = fill.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.6f, -0.4f, 0.4f));
                dl.m_Color = glm::vec3(0.5f, 0.55f, 0.7f);
                dl.m_Intensity = 1.0f;
                dl.m_CastShadows = false;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale, const glm::vec4& albedo)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh;
                switch (prim)
                {
                    case MeshPrimitive::Plane:
                        mesh = MeshPrimitives::CreatePlane();
                        break;
                    case MeshPrimitive::Sphere:
                        mesh = MeshPrimitives::CreateSphere();
                        break;
                    default:
                        mesh = MeshPrimitives::CreateCube();
                        break;
                }
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(albedo);
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.85f);
                return e;
            };

            // Grey floor.
            addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 80.0f, 1.0f, 80.0f },
                    glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));

            // A 5x5 grid of alternating bright/dark cubes: lots of high-contrast
            // silhouettes and AA'd edges — the high-frequency content CAS acts on.
            for (int row = 0; row < 5; ++row)
            {
                for (int col = 0; col < 5; ++col)
                {
                    const bool bright = ((row + col) & 1) == 0;
                    const glm::vec4 albedo = bright ? glm::vec4(0.9f, 0.88f, 0.85f, 1.0f)
                                                    : glm::vec4(0.12f, 0.13f, 0.15f, 1.0f);
                    const glm::vec3 pos = { static_cast<f32>(col - 2) * 3.0f, 1.0f,
                                            static_cast<f32>(row - 2) * 3.0f };
                    addMesh("Cube", MeshPrimitive::Cube, pos, { 1.6f, 2.0f, 1.6f }, albedo);
                }
            }

            // A couple of smooth spheres for big soft shading gradients.
            addMesh("SphereL", MeshPrimitive::Sphere, { -4.5f, 2.0f, 5.0f }, { 2.0f, 2.0f, 2.0f },
                    glm::vec4(0.8f, 0.3f, 0.25f, 1.0f));
            addMesh("SphereR", MeshPrimitive::Sphere, { 4.5f, 2.0f, 5.0f }, { 2.0f, 2.0f, 2.0f },
                    glm::vec4(0.25f, 0.45f, 0.8f, 1.0f));
        }

        // Render the current scene/settings from the given pose, read back the
        // composited frame (top-down rows), save it as PNG evidence, and verify
        // the PNG round-trips (write succeeded + reloads bit-identical).
        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for CAS capture '" << tag << "'";

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
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string()
                             << "': " << ec.message();

            const std::string path = (dir / ("CAS_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            if (w == static_cast<int>(kWidth) && h == static_cast<int>(kHeight))
            {
                EXPECT_EQ(std::memcmp(loaded, outPixels.data(),
                                      static_cast<std::size_t>(kWidth) * kHeight * 4u),
                          0)
                    << "Reloaded PNG pixels differ from the written buffer: " << path;
            }
            ::stbi_image_free(loaded);
        }
    };

    // CAS off vs on: enabling CAS must raise the frame's total gradient energy
    // (more high-frequency edge contrast) without materially shifting overall
    // brightness, and must actually change the frame. SKIPs without a GL 4.6
    // context (see header).
    TEST_F(CASVisualEvidenceTest, SharpeningIncreasesEdgeContrast)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);

        const glm::vec3 pos = { 0.0f, 7.0f, 16.0f };
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.32f;

        auto& pp = Renderer3D::GetPostProcessSettings();

        pp.CASEnabled = false;
        std::vector<u8> offPixels;
        Capture("Off", pos, yaw, pitch, offPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        pp.CASEnabled = true;
        pp.CASSharpness = 1.0f; // maximum so the differential reads clearly
        std::vector<u8> onPixels;
        Capture("On", pos, yaw, pitch, onPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        const f64 offMean = MeanLuma(offPixels);
        const f64 onMean = MeanLuma(onPixels);
        EXPECT_GT(offMean, 20.0) << "CAS-off frame rendered (near-)black";
        EXPECT_GT(onMean, 20.0) << "CAS-on frame rendered (near-)black";

        const f64 offEnergy = GradientEnergy(offPixels);
        const f64 onEnergy = GradientEnergy(onPixels);

        // Core contract: sharpening raises high-frequency energy. The scene is
        // edge-rich so the real increase is large; require a clear (>3%) margin
        // that is still comfortably above GPU/AA noise.
        EXPECT_GT(onEnergy, offEnergy * 1.03)
            << "Enabling CAS did not increase gradient energy (off=" << offEnergy
            << " on=" << onEnergy << ", ratio=" << (onEnergy / offEnergy)
            << "). See CAS_Off.png / CAS_On.png";

        // The frame must actually change (rules out a silent pass-through).
        const f64 diff = MeanAbsDiff(offPixels, onPixels);
        EXPECT_GT(diff, 0.5)
            << "CAS-on frame is essentially identical to CAS-off (mean abs luma diff=" << diff << ")";

        // Sharpening is an edge contrast boost, not a global level/gain shift:
        // mean brightness must stay close (within ~12%).
        EXPECT_LT(std::abs(onMean - offMean), offMean * 0.12)
            << "CAS shifted overall brightness too much (off=" << offMean << " on=" << onMean
            << "); it should boost edge contrast, not levels";
    }
} // namespace OloEngine::Tests
