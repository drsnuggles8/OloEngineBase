// OLO_TEST_LAYER: L8
// =============================================================================
// ClothVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for cloth / soft-body rendering (issue #460, first slice).
// Drives the REAL runtime pipeline — Scene::OnPhysics3DStart builds the Jolt soft
// bodies, then Scene::OnUpdateRuntime -> RenderScene3D steps the simulation and
// renders the deforming cloth meshes read back from Jolt — and reads the final
// composited frame back for pixel assertions + a PNG artifact.
//
// Scene: a ground plane (static box body), a key directional light, a primary
// camera, and two cloths dropped above the floor — one pinned along its top edge
// (a hanging curtain) and one free (falls and drapes onto the floor). After a few
// seconds of simulation the frame is read back to
//   OloEditor/assets/tests/visual/Cloth_VisualEvidence.png
//
// Cloth only exists during PLAY (its render mesh is built at OnPhysics3DStart and
// updated each tick from the soft-body vertex readback), so the edit-mode viewport
// — and therefore the read-only MCP screenshot path — can't show it; this runtime
// evidence test is how the draped result is eyeballed. The draping/collision
// *behaviour* is pinned headlessly by ClothSimulationTest (functional) and the
// grid math by ClothSharedSettingsTest; this adds the on-screen proof.
//
// Runs in the normal suite; SKIPs cleanly (RendererAttachedTest::SetUp) when no
// GL 4.6 context exists. The PNG is always written so a reviewer has the frame.
//
// Classification: L8 / integration (full runtime GL pipeline + physics + RGBA8
// readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Mesh.h"
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

        constexpr u32 kSize = 512;

        f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        fs::path VisualOutputPath(const char* filename = "Cloth_VisualEvidence.png")
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / filename;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // ClothScene — a ground plane + two cloths driven through the runtime path.
    // -------------------------------------------------------------------------
    class ClothScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Primary camera at a 3/4 view, tilted down to see both the hanging drape
            // and the cloth pooled on the floor.
            Entity camera = scene.CreateEntity("Camera");
            {
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 5.0f, 10.0f };
                tc.SetRotationEuler({ -0.35f, 0.0f, 0.0f }); // pitch down ~20 deg
            }
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // Key light so the cloth + floor are lit.
            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 3.0f, 8.0f, 6.0f };
            auto& dir = light.AddComponent<DirectionalLightComponent>();
            dir.m_Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.3f));
            dir.m_Color = { 1.0f, 0.97f, 0.9f };
            dir.m_Intensity = 2.0f;

            // Ground: a wide, thin static box the free cloth drapes onto. Dark material
            // so the brightly-coloured cloths stand out against it.
            Entity ground = scene.CreateEntity("Ground");
            {
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, 0.0f };
                tc.Scale = { 12.0f, 0.1f, 12.0f };
                if (Ref<Mesh> plane = MeshPrimitives::CreateCube(); plane)
                    ground.AddComponent<MeshComponent>(plane->GetMeshSource());
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.12f, 0.13f, 0.15f, 1.0f });
                auto& body = ground.AddComponent<Rigidbody3DComponent>();
                body.m_Type = BodyType3D::Static;
                auto& col = ground.AddComponent<BoxCollider3DComponent>();
                col.m_HalfExtents = { 6.0f, 0.05f, 6.0f };
            }

            // Helper to author a coloured cloth.
            auto addCloth = [&scene](const char* name, const glm::vec3& pos, ClothAttachment attach,
                                     const glm::vec4& color)
            {
                Entity cloth = scene.CreateEntity(name);
                cloth.GetComponent<TransformComponent>().Translation = pos;
                auto& c = cloth.AddComponent<ClothComponent>();
                c.m_Columns = 24;
                c.m_Rows = 24;
                c.m_Width = 3.0f;
                c.m_Height = 3.0f;
                c.m_Attachment = attach;
                c.m_Iterations = 6;
                cloth.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(color);
            };

            // Right: a static box for the free cloth to drape over — turns the flat
            // "lands on the floor" case into a 3D tent that faces the camera (and is a
            // clearer soft-vs-rigid collision demo than a flat floor).
            {
                Entity box = scene.CreateEntity("DrapeBox");
                auto& tc = box.GetComponent<TransformComponent>();
                tc.Translation = { 2.2f, 0.7f, 0.0f };
                tc.Scale = { 1.4f, 1.4f, 1.4f };
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube(); cube)
                    box.AddComponent<MeshComponent>(cube->GetMeshSource());
                box.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.18f, 0.19f, 0.2f, 1.0f });
                auto& body = box.AddComponent<Rigidbody3DComponent>();
                body.m_Type = BodyType3D::Static;
                auto& col = box.AddComponent<BoxCollider3DComponent>();
                col.m_HalfExtents = { 0.7f, 0.7f, 0.7f };
            }

            // Left: pinned along the top edge — a hanging red curtain.
            addCloth("HangingCloth", { -2.2f, 4.5f, 0.0f }, ClothAttachment::TopEdge, { 0.85f, 0.12f, 0.12f, 1.0f });
            // Right: free — a blue sheet that falls and drapes over the box.
            addCloth("FallingCloth", { 2.2f, 4.5f, 0.0f }, ClothAttachment::None, { 0.15f, 0.3f, 0.9f, 1.0f });

            EnableRendering(kSize, kSize);

            // Build the Jolt soft bodies + cloth render state (mirrors the runtime start
            // path the editor's Play button drives). RunFrames then steps + renders them.
            scene.OnPhysics3DStart();
        }
    };

    TEST_F(ClothScene, RendersDrapingClothThroughRuntimePipeline)
    {
        // ~2.5 s of simulation so the free cloth reaches the floor and the pinned
        // cloth settles into its hanging drape.
        RunFrames(150);

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        EXPECT_EQ(width, kSize);
        EXPECT_EQ(height, kSize);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

        // Always write the PNG first so the artifact survives a later failed assert.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(width), static_cast<int>(height),
                                           4, px.data(), static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        // Contrast contract: a flat/black frame (nothing drawn) has ~zero luminance
        // spread. The lit cloth + floor must produce real contrast.
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
            << ") — cloth/floor may not have drawn; see " << out.string();
        EXPECT_GT(maxLum, 0.10f)
            << "rendered frame has no bright pixels — pipeline may have output black; see "
            << out.string();

        // Colour contract (driver-independent): the two cloths are authored bright red
        // and bright blue against a dark floor/background, so a correctly-rendered frame
        // must contain clearly red-dominant AND clearly blue-dominant pixels. If the cloth
        // meshes didn't draw (e.g. culled, never built, or not submitted), neither colour
        // appears. Threshold is a small fraction of the frame so a thin drape still counts.
        std::size_t redPixels = 0;
        std::size_t bluePixels = 0;
        for (std::size_t i = 0; i + 3 < px.size(); i += 4)
        {
            const int r = px[i + 0];
            const int g = px[i + 1];
            const int b = px[i + 2];
            if (r > 110 && r - g > 55 && r - b > 55)
                ++redPixels;
            if (b > 110 && b - r > 45 && b - g > 25)
                ++bluePixels;
        }
        const std::size_t totalPixels = static_cast<std::size_t>(width) * height;
        const std::size_t minCloth = totalPixels / 400; // ~0.25% of the frame
        EXPECT_GT(redPixels, minCloth)
            << "no red cloth pixels (" << redPixels << ") — the hanging cloth may not have rendered; see "
            << out.string();
        EXPECT_GT(bluePixels, minCloth)
            << "no blue cloth pixels (" << bluePixels << ") — the falling cloth may not have rendered; see "
            << out.string();

        // Behaviour cross-check (driver-independent): both cloths are live and have
        // fallen from their spawn height (y = 4.5) under gravity.
        const auto clothFellBelow = [this](const char* tag, f32 threshold)
        {
            Entity e = GetScene().FindEntityByName(tag);
            ASSERT_TRUE(e) << "missing cloth entity '" << tag << "'";
            const std::vector<glm::vec3>* verts = GetScene().GetClothVertexPositions(e.GetUUID());
            ASSERT_NE(verts, nullptr) << "cloth '" << tag << "' has no live soft body";
            f32 minY = std::numeric_limits<f32>::max();
            for (const glm::vec3& p : *verts)
                minY = std::min(minY, p.y);
            EXPECT_LT(minY, threshold) << "cloth '" << tag << "' did not fall; minY=" << minY;
        };
        clothFellBelow("HangingCloth", 4.0f); // pinned edge holds ~4.5, free part sags below 4.0
        clothFellBelow("FallingCloth", 2.0f); // free cloth drops well down toward the floor
    }

    // -------------------------------------------------------------------------
    // ClothWindScene — a single pinned cloth under a strong, steady sideways wind,
    // proving the WindSystem -> ClothWindSystem -> Jolt soft-body force coupling
    // visually (issue #460, wind-coupling slice). Companion to ClothScene above,
    // which only exercises gravity; the billow/displacement behaviour itself is
    // pinned headlessly by ClothWindTest in ClothSimulationTest.cpp — this adds
    // the on-screen proof.
    // -------------------------------------------------------------------------
    class ClothWindScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            // Same camera framing as ClothScene above (proven to light/frame a hanging
            // TopEdge cloth well).
            Entity camera = scene.CreateEntity("Camera");
            {
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 5.0f, 10.0f };
                tc.SetRotationEuler({ -0.35f, 0.0f, 0.0f });
            }
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity light = scene.CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 3.0f, 8.0f, 6.0f };
            auto& dir = light.AddComponent<DirectionalLightComponent>();
            // Brighter and more forward-facing than ClothScene's overhead-biased light.
            // This single-cloth scene renders essentially unlit black with EITHER
            // ClothScene's light alone OR the ground plane below alone (confirmed by A/B
            // screenshot — issue #460 wind-coupling slice); both this light tweak AND the
            // ground are needed together to light a lone hanging cloth's vertical face.
            dir.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.6f, -0.75f));
            dir.m_Color = { 1.0f, 0.97f, 0.9f };
            dir.m_Intensity = 3.0f;

            // Ground: not for the cloth to physically rest on (no collider here; this
            // cloth never reaches the floor) but required alongside the light tweak above
            // for correct lighting — see the note there. Dark, deliberately unremarkable
            // material so it doesn't compete with the cloth for visual attention.
            Entity ground = scene.CreateEntity("Ground");
            {
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, 0.0f };
                tc.Scale = { 12.0f, 0.1f, 12.0f };
                if (Ref<Mesh> plane = MeshPrimitives::CreateCube(); plane)
                    ground.AddComponent<MeshComponent>(plane->GetMeshSource());
                ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.12f, 0.13f, 0.15f, 1.0f });
            }

            // Moderate, steady sideways wind (+X world == screen-horizontal from this
            // camera). No gust noise so the billow reads as a clean, deterministic shift.
            WindSettings& wind = scene.GetWindSettings();
            wind.Enabled = true;
            wind.Direction = { 1.0f, 0.0f, 0.0f };
            wind.Speed = 10.0f;
            wind.GustStrength = 0.0f;

            // A single hanging cloth, pinned along its top edge (same attachment as
            // ClothScene's proven-good drape), centred so it has room to billow sideways
            // on screen. A saturated orange stays legible even under a grazing key light,
            // unlike a muted amber which reads as near-black.
            m_Cloth = scene.CreateEntity("WindCloth");
            m_Cloth.GetComponent<TransformComponent>().Translation = { 0.0f, 4.5f, 0.0f };
            auto& c = m_Cloth.AddComponent<ClothComponent>();
            c.m_Columns = 24;
            c.m_Rows = 24;
            c.m_Width = 3.0f;
            c.m_Height = 3.0f;
            c.m_Attachment = ClothAttachment::TopEdge;
            c.m_Iterations = 6;
            c.m_WindInfluence = 1.0f;
            m_Cloth.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor({ 0.95f, 0.5f, 0.05f, 1.0f });

            EnableRendering(kSize, kSize);

            // Build the Jolt soft body + cloth render state (mirrors the runtime start path
            // the editor's Play button drives). RunFrames then steps + renders it.
            scene.OnPhysics3DStart();
        }

        Entity m_Cloth;
    };

    TEST_F(ClothWindScene, WindVisiblyBillowsHangingClothSideways)
    {
        auto averageX = [](const std::vector<glm::vec3>& positions) -> f32
        {
            f32 sum = 0.0f;
            for (const glm::vec3& p : positions)
                sum += p.x;
            return positions.empty() ? 0.0f : sum / static_cast<f32>(positions.size());
        };

        const UUID clothID = m_Cloth.GetUUID();

        const std::vector<glm::vec3>* initial = GetScene().GetClothVertexPositions(clothID);
        ASSERT_NE(initial, nullptr) << "cloth soft body was not created / has no readback";
        ASSERT_GT(initial->size(), 0u);
        const f32 startAvgX = averageX(*initial);

        // ~1.5 s of simulation — enough for a clean, visible billow without the
        // sheet fully winding up into a twisted/edge-on silhouette.
        RunFrames(90);

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        EXPECT_EQ(width, kSize);
        EXPECT_EQ(height, kSize);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

        // Always write the PNG first so the artifact survives a later failed assert.
        const fs::path out = VisualOutputPath("Cloth_WindVisualEvidence.png");
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(width), static_cast<int>(height),
                                           4, px.data(), static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write wind visual evidence PNG to " << out.string();

        // Contrast contract: a flat/black frame (nothing drawn) has ~zero luminance spread.
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
            << ") — cloth may not have drawn; see " << out.string();

        // Behaviour cross-check (driver-independent): the cloth billowed measurably
        // downwind (+X) from where it spawned, under the real Jolt simulation.
        const std::vector<glm::vec3>* settled = GetScene().GetClothVertexPositions(clothID);
        ASSERT_NE(settled, nullptr);
        const f32 endAvgX = averageX(*settled);
        EXPECT_GT(endAvgX - startAvgX, 0.05f)
            << "cloth did not billow sideways under wind; start=" << startAvgX << " end=" << endAvgX;
    }
} // namespace OloEngine::Tests
