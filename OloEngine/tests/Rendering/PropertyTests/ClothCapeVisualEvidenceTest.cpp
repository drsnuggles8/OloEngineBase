// OLO_TEST_LAYER: L8
// =============================================================================
// ClothCapeVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the skinned cape / cloth skeleton-attachment slice
// (issue #460, cape slice — the cloth epic's final acceptance criterion:
// "a cape attached to an animated character moves believably").
//
// Drives the REAL runtime pipeline — Scene::OnPhysics3DStart builds the Jolt soft
// body, then Scene::OnUpdateRuntime steps the sim (gravity + the per-tick
// Scene::DriveClothAttachments kinematic bone drive) and renders the deforming
// cape read back from Jolt. A visible "character" (a box) is swung side to side
// each frame; the cape is welded to it via ClothComponent::m_AttachmentEntity, so
// on-screen the cape's top edge tracks the character while the rest trails and
// billows under gravity. The frame is read back for pixel assertions + PNGs to
//   OloEditor/assets/tests/visual/ClothCape_*.png
// from two camera angles (a 3/4 hero view and a side view), because "moves
// believably" is a visual judgement and a plausible-but-wrong result (cape
// detaches, freezes rigid, inverts) is exactly the silent failure CLAUDE.md warns
// about. The FOLLOW behaviour itself is pinned headlessly by
// ClothSkeletonAttachTest (functional); this adds the on-screen proof.
//
// Runs in the normal suite; SKIPs cleanly (RendererAttachedTest::SetUp) when no
// GL 4.6 context exists. The PNGs are always written so a reviewer has the frames.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 512;

        fs::path VisualOutputPath(const char* filename)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / filename;
        }

        // Count the cape's bright-magenta pixels — a colour nothing else in the scene
        // wears — so "the cape drew" is a driver-independent check.
        std::size_t MagentaPixels(const std::vector<u8>& px)
        {
            std::size_t n = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const int r = px[i + 0];
                const int g = px[i + 1];
                const int b = px[i + 2];
                if (r > 110 && b > 110 && r - g > 55 && b - g > 55)
                    ++n;
            }
            return n;
        }

        void WritePng(const fs::path& out, const std::vector<u8>& px, u32 w, u32 h)
        {
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(w),
                                               static_cast<int>(h), 4, px.data(), static_cast<int>(w) * 4);
            EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();
        }
    } // namespace

    // -------------------------------------------------------------------------
    // ClothCapeScene — a ground plane, a swinging "character" box, and a cape
    // welded to it via ClothComponent skeleton attachment (bone name empty → the
    // cape follows the character entity's own world transform, the socket case).
    // -------------------------------------------------------------------------
    class ClothCapeScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Primary camera — a 3/4 hero view of the character + trailing cape.
            Entity camera = scene.CreateEntity("Camera");
            {
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 4.5f, 9.0f };
                tc.SetRotationEuler({ -0.15f, 0.0f, 0.0f });
            }
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 3.0f, 8.0f, 6.0f };
            auto& dir = light.AddComponent<DirectionalLightComponent>();
            dir.m_Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.3f));
            dir.m_Color = { 1.0f, 0.97f, 0.9f };
            dir.m_Intensity = 2.0f;

            // Ground plane so the scene reads as a lit space, not a void.
            Entity ground = scene.CreateEntity("Ground");
            {
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, 0.0f };
                tc.Scale = { 14.0f, 0.1f, 14.0f };
                if (Ref<Mesh> plane = MeshPrimitives::CreateCube(); plane)
                    ground.AddComponent<MeshComponent>(plane->GetMeshSource());
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.12f, 0.13f, 0.15f, 1.0f });
            }

            // The "character": a visible box we swing side to side each frame. A pale
            // material so it reads distinctly from the bright cape.
            m_Character = scene.CreateEntity("Character");
            {
                auto& tc = m_Character.GetComponent<TransformComponent>();
                tc.Translation = kCharacterHome;
                tc.Scale = { 1.2f, 1.6f, 0.6f };
                if (Ref<Mesh> body = MeshPrimitives::CreateCube(); body)
                    m_Character.AddComponent<MeshComponent>(body->GetMeshSource());
                m_Character.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.7f, 0.72f, 0.75f, 1.0f });
            }

            // The cape: pinned along its top edge and welded to the character. Authored so
            // its top edge sits at the character's shoulders; gravity drapes the rest.
            m_Cape = scene.CreateEntity("Cape");
            {
                // In FRONT of the character (higher Z, closer to the camera) so the
                // draped cape is never occluded by the character box body.
                m_Cape.GetComponent<TransformComponent>().Translation = { 0.0f, 4.6f, 0.2f };
                auto& c = m_Cape.AddComponent<ClothComponent>();
                c.m_Columns = 24;
                c.m_Rows = 24;
                c.m_Width = 2.4f;
                c.m_Height = 2.6f;
                c.m_Mass = 1.0f;
                c.m_Iterations = 8;
                c.m_Attachment = ClothAttachment::TopEdge;
                c.m_AttachmentEntity = m_Character.GetUUID(); // follow the character
                c.m_AttachmentBone = "";                      // no skeleton → the entity transform
                m_Cape.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.85f, 0.1f, 0.8f, 1.0f });
            }

            EnableRendering(kSize, kSize);

            // Build the Jolt soft body + cloth render state (the runtime-start path the
            // editor Play button drives). This resolves the cape's attachment weld.
            scene.OnPhysics3DStart();
        }

        // Swing the character along X by `amplitude` over `frames`, stepping + rendering
        // one runtime frame per position so the cape follows and trails behind it.
        void SwingCharacter(f32 amplitude, u32 frames)
        {
            for (u32 f = 0; f < frames; ++f)
            {
                const f32 phase = static_cast<f32>(f) / static_cast<f32>(frames) * 2.0f * 3.14159265f;
                glm::vec3 pos = kCharacterHome;
                pos.x += amplitude * std::sin(phase);
                m_Character.GetComponent<TransformComponent>().Translation = pos;
                RunFrames(1);
            }
        }

        static constexpr glm::vec3 kCharacterHome{ 0.0f, 4.0f, -0.8f };

        Entity m_Character;
        Entity m_Cape;
    };

    TEST_F(ClothCapeScene, CapeFollowsSwingingCharacterThroughRuntimePipeline)
    {
        // Let the cape drape from its welded edge first.
        RunFrames(90);
        // Then swing the character a couple of cycles so the cape visibly trails.
        SwingCharacter(2.2f, 120);
        // Leave the character parked off-centre and let the cape catch up + re-drape.
        // A generous settle (matching the other cloth visual tests) so the free part
        // fully hangs again after being flung by the swing — a clean hero shot with
        // obvious sideways displacement AND a deep drape.
        {
            glm::vec3 pos = kCharacterHome;
            pos.x += 2.2f;
            m_Character.GetComponent<TransformComponent>().Translation = pos;
        }
        RunFrames(180);

        // ── Hero-angle readback + PNG ──
        std::vector<u8> hero;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(hero, width, height)) << "ReadbackComposite failed";
        EXPECT_EQ(width, kSize);
        EXPECT_EQ(height, kSize);
        ASSERT_EQ(hero.size(), static_cast<std::size_t>(width) * height * 4u);
        WritePng(VisualOutputPath("ClothCape_Hero.png"), hero, width, height);

        // ── Side-angle readback + PNG (multi-angle per CLAUDE.md rendering rules) ──
        // OnUpdateEditor doesn't step physics, so the cape is frozen at its last runtime
        // pose — exactly what we want for a still from a fresh angle.
        std::vector<u8> side;
        {
            EditorCamera sideCam(60.0f, 1.0f, 0.05f, 1000.0f);
            sideCam.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
            // Look at the character/cape from its +X side.
            sideCam.SetPose(glm::vec3(9.0f, 4.2f, 0.0f), -3.14159265f * 0.5f, -0.1f);
            RunEditorFrames(sideCam, 2);
            u32 sw = 0;
            u32 sh = 0;
            ASSERT_TRUE(ReadbackComposite(side, sw, sh));
            WritePng(VisualOutputPath("ClothCape_Side.png"), side, sw, sh);
        }

        // Contract 1 (driver-independent): the magenta cape actually drew in the hero shot.
        const std::size_t capePixels = MagentaPixels(hero);
        const std::size_t totalPixels = static_cast<std::size_t>(width) * height;
        EXPECT_GT(capePixels, totalPixels / 400)
            << "no magenta cape pixels (" << capePixels << ") — the cape may not have rendered; see "
            << VisualOutputPath("ClothCape_Hero.png").string();

        // Contract 2 (behaviour cross-check, driver-independent): the cape is live and
        // FOLLOWED the character to its parked +X position — its pinned top edge sits well
        // to the +X side of the origin, and the free part still hangs below that edge.
        const std::vector<glm::vec3>* verts = GetScene().GetClothVertexPositions(m_Cape.GetUUID());
        ASSERT_NE(verts, nullptr) << "cape has no live soft body";
        glm::vec3 topSum(0.0f);
        f32 minY = std::numeric_limits<f32>::max();
        bool allFinite = true;
        const u32 columns = 24;
        for (std::size_t i = 0; i < verts->size(); ++i)
        {
            const glm::vec3& p = (*verts)[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                allFinite = false;
            if (i < columns)
                topSum += p;
            minY = std::min(minY, p.y);
        }
        const glm::vec3 topAvg = topSum / static_cast<f32>(columns);
        EXPECT_TRUE(allFinite) << "cape vertices contain NaN/Inf";
        EXPECT_GT(topAvg.x, 1.2f)
            << "cape's welded top edge did not follow the character to +X; topAvgX=" << topAvg.x;
        EXPECT_LT(minY, topAvg.y - 0.4f)
            << "cape went rigid (free part stopped hanging below its edge); minY=" << minY
            << " topAvgY=" << topAvg.y;
    }
} // namespace OloEngine::Tests
