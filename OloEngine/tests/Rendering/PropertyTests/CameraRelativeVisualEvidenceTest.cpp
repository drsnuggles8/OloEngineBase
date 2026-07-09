// OLO_TEST_LAYER: L8
// =============================================================================
// Visual evidence for camera-relative rendering (issue #429).
//
// The acceptance bar for the first slice is: no visible vertex jitter / shadow
// swim for geometry placed far (10-50 km) from the world origin, shown by an
// actual screenshot before/after — not just "the math should work".
//
// This test renders the SAME sphere-on-plane-under-a-shadow-casting-sun scene
// three ways and reads the pixels back:
//   * near_ref : built at the world origin — the ground-truth appearance (the
//                feature is a no-op within the first grid cell, so this is what
//                the frame is *supposed* to look like).
//   * far_on   : built at ~45 km with camera-relative rendering ON (the fix).
//   * far_off  : the same 45 km scene with the feature forced OFF (the pre-#429
//                world-space path — the "before").
// Because far_on and near_ref render the identical local geometry near 0, they
// should match closely; far_off, computing everything in f32 at 45 km, loses
// fine vertex detail and mislocates the shadow, so it diverges from the truth.
// The test asserts far_on is much closer to near_ref than far_off is, and writes
// all three PNGs to assets/tests/visual/ for direct inspection.
//
// SKIPs cleanly (does not fail) when no GL 4.6 context exists — same gate as the
// other RendererAttachedTest visual-evidence tests, so it guards GPU-equipped
// runs while headless CI skips it.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
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

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // Mean RMSE over RGB (alpha ignored) between two equal-size RGBA8 buffers.
        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        // Fraction of pixels that are not the (dark) clear colour — a cheap
        // "something was actually drawn" check so a blank frame can't pass.
        [[nodiscard]] f64 NonClearFraction(const std::vector<u8>& px)
        {
            std::size_t nonClear = 0, total = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                if (px[i] > 12 || px[i + 1] > 12 || px[i + 2] > 12)
                    ++nonClear;
                ++total;
            }
            return total ? static_cast<f64>(nonClear) / static_cast<f64>(total) : 0.0;
        }
    } // namespace

    class CameraRelativeVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // Build geometry in the test body (not here) so the same scene can be
        // rebuilt at different world centres. BuildScene only opts into the full
        // 3D draw path and sizes the render-graph targets.
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
        }

        // A shadow-casting sun + a fine-tessellated sphere resting on a plane,
        // all offset by `center`. The sphere's 64-segment tessellation makes the
        // f32 vertex quantization at 45 km visible as facetting; the plane +
        // shadow make shadow mislocation visible.
        void PopulateScene(const glm::vec3& center, std::vector<Entity>& out)
        {
            Scene& scene = GetScene();

            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.9f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = true;
                out.push_back(sun);
            }

            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = center + glm::vec3(0.0f, 0.0f, 0.0f);
                tc.Scale = { 40.0f, 1.0f, 40.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.5f, 0.5f, 0.55f, 1.0f));
                out.push_back(ground);
            }

            {
                Entity sphere = scene.CreateEntity("Sphere");
                auto& tc = sphere.GetComponent<TransformComponent>();
                tc.Translation = center + glm::vec3(0.0f, 2.0f, 0.0f);
                tc.Scale = { 2.0f, 2.0f, 2.0f };
                auto& mc = sphere.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere(1.0f, 64))
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = sphere.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.3f, 0.2f, 1.0f));
                out.push_back(sphere);
            }
        }

        void ClearEntities(std::vector<Entity>& ents)
        {
            Scene& scene = GetScene();
            for (Entity e : ents)
                scene.DestroyEntity(e);
            ents.clear();
        }

        // Pose an EditorCamera at `eye` looking along (yaw,pitch), render the
        // full editor pipeline, read back the composited frame (top-row-first),
        // and save it to assets/tests/visual/CameraRelative_<tag>.png.
        void Capture(const std::string& tag, const glm::vec3& eye, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip to top-row-first for the PNG.
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
            const std::string path = (dir / ("CameraRelative_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, outPixels.data(), static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
            OLO_CORE_INFO("CameraRelativeVisualEvidence: wrote {} (abs: {})", path, fs::absolute(path).string());
        }
    };

    // Far-origin geometry must render the same as at the origin with the feature
    // ON, and visibly worse with it OFF. SKIPs without a GL 4.6 context.
    TEST_F(CameraRelativeVisualEvidenceTest, FarOriginMatchesNearOriginWithFeatureOnNotOff)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The sphere sits at center + (0,2,0); frame it from slightly above and in
        // front, looking down at the sphere + its ground shadow. The eye offset is
        // the SAME relative to each scene centre, so near_ref and far_* render the
        // identical local geometry — only the world offset differs.
        const glm::vec3 eyeOffset(0.0f, 4.0f, 9.0f);
        const f32 yaw = 0.0f;    // look toward -Z (toward the sphere)
        const f32 pitch = 0.30f; // tilt down onto the ground shadow

        std::vector<Entity> ents;

        // --- Ground truth: identical scene at the world origin (feature is a
        //     no-op here, so this is the correct appearance). ---
        Renderer3D::SetCameraRelativeEnabled(true);
        std::vector<u8> nearRef;
        PopulateScene(glm::vec3(0.0f), ents);
        Capture("near_ref", eyeOffset, yaw, pitch, nearRef);
        ClearEntities(ents);

        // --- The same scene ~45 km from the origin. ---
        const glm::vec3 farCenter(45000.0f, 0.0f, -45000.0f);
        PopulateScene(farCenter, ents);

        // Feature ON — should reproduce the near-origin appearance.
        Renderer3D::SetCameraRelativeEnabled(true);
        std::vector<u8> farOn;
        Capture("far_on", farCenter + eyeOffset, yaw, pitch, farOn);

        // Feature OFF — the pre-#429 world-space path; large-coordinate jitter.
        Renderer3D::SetCameraRelativeEnabled(false);
        std::vector<u8> farOff;
        Capture("far_off", farCenter + eyeOffset, yaw, pitch, farOff);

        Renderer3D::SetCameraRelativeEnabled(true); // restore for later tests
        ClearEntities(ents);

        // Sanity: the reference and the on-frame actually drew the scene.
        EXPECT_GT(NonClearFraction(nearRef), 0.10) << "near-origin reference frame looks blank";
        EXPECT_GT(NonClearFraction(farOn), 0.10) << "far-origin (feature on) frame looks blank";

        const f64 rmseOnVsRef = Rgba8Rmse(farOn, nearRef);
        const f64 rmseOffVsRef = Rgba8Rmse(farOff, nearRef);

        OLO_CORE_INFO("CameraRelativeVisualEvidence: RMSE far_on-vs-ref={:.3f}, far_off-vs-ref={:.3f}",
                      rmseOnVsRef, rmseOffVsRef);

        // With the feature ON the 45 km frame closely reproduces the origin frame.
        EXPECT_LT(rmseOnVsRef, 8.0)
            << "camera-relative rendering should reproduce the near-origin appearance at 45 km (RMSE "
            << rmseOnVsRef << ")";
        // With it OFF the world-space path visibly degrades the frame, so it is
        // markedly further from the truth than the ON frame.
        EXPECT_GT(rmseOffVsRef, rmseOnVsRef * 2.0)
            << "feature-off 45 km frame should be much further from truth than feature-on (on="
            << rmseOnVsRef << ", off=" << rmseOffVsRef << ")";
    }
} // namespace OloEngine::Tests
