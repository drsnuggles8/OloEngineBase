// OLO_TEST_LAYER: L8
// =============================================================================
// Visual evidence for floating-origin / origin-rebasing (issue #429).
//
// The failure mode CLAUDE.md warns about for this work is a VISIBLE pop/jitter
// at the rebase boundary: the instant the world shifts back toward the origin,
// a bug in the shift (geometry moved but the camera didn't, or vice versa, or a
// subset of entities left behind) would show as a hard jump in the rendered
// frame. Math/contract tests (WorldOriginRebaseTest) prove the arithmetic; only
// rendering the real pipeline before and after a rebase and comparing the pixels
// proves the frame doesn't pop.
//
// This test renders the SAME sphere-on-plane-under-a-sun scene twice:
//   * far_before : geometry built ~50 km from the origin (the 50 km² map scale
//                  in issue #429's acceptance bar), camera posed relative to it,
//                  camera-relative rendering ON — the frame BEFORE a rebase fires.
//   * rebased_after : Scene::RebaseOrigin shifts the whole world (geometry) back
//                  to the origin; the camera is re-posed by the same relative
//                  offset (as it would be, since it tracks the player who also
//                  shifted). The frame the moment AFTER the rebase.
// A correct rebase translates everything together, so the two frames must be
// pixel-near-identical — that IS the "no visible pop" guarantee. Both PNGs are
// written to assets/tests/visual/ for direct inspection.
//
// SKIPs cleanly (does not fail) when no GL 4.6 context exists — same gate as the
// sibling camera-relative visual-evidence test.
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

#include <algorithm>
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

    class WorldOriginRebaseVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
        }

        // A shadow-casting sun + a fine-tessellated sphere on a plane, offset by
        // `center`. Mirrors the camera-relative evidence scene so the two tests
        // are directly comparable.
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
                tc.Translation = center;
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
            const std::string path = (dir / ("WorldOriginRebase_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, outPixels.data(), static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
            OLO_CORE_INFO("WorldOriginRebaseVisualEvidence: wrote {} (abs: {})", path, fs::absolute(path).string());
        }
    };

    // The frame immediately before and immediately after a rebase must match:
    // the whole world (geometry + camera) translates together, so a correct
    // rebase is visually invisible. A pop would mean the shift left something
    // behind. SKIPs without a GL 4.6 context.
    TEST_F(WorldOriginRebaseVisualEvidenceTest, RebaseProducesNoVisiblePop)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eyeOffset(0.0f, 4.0f, 9.0f);
        const f32 yaw = 0.0f;
        const f32 pitch = 0.30f;

        const glm::vec3 farCenter(50000.0f, 0.0f, -50000.0f);

        std::vector<Entity> ents;
        PopulateScene(farCenter, ents);

        // Frame the moment BEFORE the rebase: geometry at 45 km, camera posed
        // relative to it, camera-relative rendering ON (the shipped default).
        Renderer3D::SetCameraRelativeEnabled(true);
        std::vector<u8> farBefore;
        Capture("far_before", farCenter + eyeOffset, yaw, pitch, farBefore);

        // Rebase the whole world back to the origin. Everything the frame reads
        // (root transforms → world matrices) moves by exactly this shift.
        GetScene().RebaseOrigin(-farCenter);

        // The geometry now sits at the origin; the camera tracks the player, who
        // shifted by the same delta, so it re-poses at the SAME relative offset.
        std::vector<u8> rebasedAfter;
        Capture("rebased_after", eyeOffset, yaw, pitch, rebasedAfter);

        ClearEntities(ents);

        // Both frames actually drew the scene.
        EXPECT_GT(NonClearFraction(farBefore), 0.10) << "before-rebase frame looks blank";
        EXPECT_GT(NonClearFraction(rebasedAfter), 0.10) << "after-rebase frame looks blank";

        const f64 rmse = Rgba8Rmse(farBefore, rebasedAfter);
        OLO_CORE_INFO("WorldOriginRebaseVisualEvidence: RMSE before-vs-after={:.3f}", rmse);

        // No visible pop: the pixels barely move. (A small non-zero RMSE is
        // expected — the 50 km frame carries a touch of residual f32 noise the
        // origin frame doesn't, exactly the precision the rebase recovers.)
        EXPECT_LT(rmse, 5.0)
            << "the rebase produced a visible pop — before/after frames diverge (RMSE " << rmse << ")";
    }

    // A full traversal fires MANY rebases; the no-pop guarantee must hold at every
    // boundary, not just once. This walks the geometry from ~50 km back toward the
    // origin in several hops, rebasing between each and re-posing the camera at the
    // same relative offset (as it tracks the player who shifted too). Every
    // consecutive pair of frames must be pixel-near-identical — the 50 km² traversal
    // acceptance bar rendered as a continuity check. SKIPs without a GL 4.6 context.
    TEST_F(WorldOriginRebaseVisualEvidenceTest, MultipleSequentialRebasesProduceNoVisiblePop)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eyeOffset(0.0f, 4.0f, 9.0f);
        const f32 yaw = 0.0f;
        const f32 pitch = 0.30f;

        Renderer3D::SetCameraRelativeEnabled(true);

        glm::vec3 center(50000.0f, 0.0f, -50000.0f);
        std::vector<Entity> ents;
        PopulateScene(center, ents);

        // 4 waypoints, 3 rebases. Each rebase hops the whole world a big chunk back
        // toward the origin; the camera re-poses at the same relative offset.
        const glm::vec3 rebaseStep(-16000.0f, 0.0f, 16000.0f);
        std::vector<u8> prev;
        f64 worstRmse = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            std::vector<u8> frame;
            Capture("traversal_" + std::to_string(i), center + eyeOffset, yaw, pitch, frame);
            EXPECT_GT(NonClearFraction(frame), 0.10) << "traversal frame " << i << " looks blank";

            if (!prev.empty())
            {
                const f64 rmse = Rgba8Rmse(prev, frame);
                worstRmse = std::max(worstRmse, rmse);
                EXPECT_LT(rmse, 5.0)
                    << "visible pop at rebase boundary " << i << " (RMSE " << rmse << ")";
            }
            prev = frame;

            if (i < 3)
            {
                GetScene().RebaseOrigin(rebaseStep);
                center += rebaseStep; // geometry + tracked camera moved with the world
            }
        }

        ClearEntities(ents);
        OLO_CORE_INFO("WorldOriginRebaseVisualEvidence: worst consecutive RMSE across 3 rebases={:.3f}",
                      worstRmse);
    }
} // namespace OloEngine::Tests
