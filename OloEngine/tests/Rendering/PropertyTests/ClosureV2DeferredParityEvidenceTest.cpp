// OLO_TEST_LAYER: L8
// =============================================================================
// ClosureV2DeferredParityEvidenceTest.cpp — the PBRModel selector must be LIVE
// on BOTH raster paths (issue #975).
//
// This test exists because live-editor verification caught exactly the bug it
// pins: the deferred path decoded the closure-model bit out of the G-Buffer
// flags lane and then never passed it into the lighting calls, so every
// ClosureV2 material silently shaded Legacy on Deferred while Forward looked
// perfect — and the whole headless suite stayed green, because nothing
// compared the selector across the two paths. The model's journey differs
// completely per path (Forward: material UBO -> u_PBRModel parameter;
// Deferred: u_PBRModel * 2 -> G-Buffer RT2 alpha bit 1 -> ComputeDeferredLit
// decode), so each hop is its own silent-failure opportunity.
//
// Scene: two IDENTICAL near-mirror metal spheres, differing ONLY in
// Material::PBRModel, under one directional light. Near-mirror metal is the
// maximally discriminating material: the Legacy closure's EPSILON-clamped NDF
// collapses its specular to ~nothing (documented in ReferenceBRDF.h), while
// ClosureV2's alpha-clamped lobe puts a bright highlight somewhere on a sphere
// for ANY camera/light pair. On each path the v2 sphere's peak luminance must
// beat the Legacy sphere's by a wide margin — the lane-dead regression makes
// the two spheres identical and fails the assertion on that path.
//
// Evidence PNGs (written before any assertion):
//   OloEditor/assets/tests/visual/ClosureV2Parity_Forward.png
//   OloEditor/assets/tests/visual/ClosureV2Parity_Deferred.png
//
// Classification: L8 (full Scene pipeline on both raster paths, RGBA8
// readback + PNG; SKIPs cleanly without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 384;

        [[nodiscard]] f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        // Peak luminance over one horizontal half of the frame, excluding a
        // small border. The PEAK (not the mean) is the discriminator: the v2
        // near-mirror highlight is small and bright, and a mean would dilute
        // it toward the shared ambient/background level.
        [[nodiscard]] f32 PeakLuminanceInHalf(const std::vector<u8>& px, u32 w, u32 h, bool rightHalf)
        {
            const u32 x0 = rightHalf ? w / 2 : 8u;
            const u32 x1 = rightHalf ? w - 8u : w / 2;
            f32 peak = 0.0f;
            for (u32 y = 8; y < h - 8; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    peak = std::max(peak, LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4));
                }
            }
            return peak;
        }

        [[nodiscard]] fs::path VisualOutputPath(const char* pathName)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (std::string("ClosureV2Parity_") + pathName + ".png");
        }
    } // namespace

    class ClosureV2DeferredParityScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 4.5f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // Directional key light, tilted so the mirror highlight lands on
            // the camera-facing hemisphere of each sphere.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = { -0.3f, -0.5f, -0.8f };
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = 3.0f;
            dirLight.m_CastShadows = false;

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 32);

            // LEFT: Legacy near-mirror metal (constructor-default model).
            {
                Entity entity = GetScene().CreateEntity("LegacyMirror");
                entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
                entity.GetComponent<TransformComponent>().Translation = { -1.4f, 0.0f, 0.0f };
                auto& materialComp = entity.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.93f, 0.9f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(1.0f);
                materialComp.m_Material.SetRoughnessFactor(0.02f);
            }

            // RIGHT: the identical material, opted into ClosureV2.
            {
                Entity entity = GetScene().CreateEntity("ClosureV2Mirror");
                entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
                entity.GetComponent<TransformComponent>().Translation = { 1.4f, 0.0f, 0.0f };
                auto& materialComp = entity.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.93f, 0.9f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(1.0f);
                materialComp.m_Material.SetRoughnessFactor(0.02f);
                materialComp.m_Material.SetPBRModel(PBRModel::ClosureV2);
            }

            EnableRendering(kSize, kSize);
        }

        // Renders the scene on the given path and asserts the v2 sphere's
        // specular peak dwarfs the Legacy one's. The fixture's TearDown
        // restores RendererSettings, so the path switch cannot leak.
        void ExpectV2BeatsLegacyOnPath(RenderingPath path, const char* pathName)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            RunFrames(2);

            std::vector<u8> px;
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(px, width, height))
                << pathName << ": ReadbackComposite failed";
            ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

            const fs::path out = VisualOutputPath(pathName);
            const int wrote = ::stbi_write_png(out.string().c_str(),
                                               static_cast<int>(width), static_cast<int>(height),
                                               4, px.data(), static_cast<int>(width) * 4);
            EXPECT_NE(wrote, 0) << "failed to write " << out.string();

            const f32 legacyPeak = PeakLuminanceInHalf(px, width, height, /*rightHalf=*/false);
            const f32 v2Peak = PeakLuminanceInHalf(px, width, height, /*rightHalf=*/true);

            // Non-vacuity: something rendered on both halves (the spheres are
            // lit dielectric-grey at worst; an empty half reads background).
            EXPECT_GT(legacyPeak, 0.02f) << pathName << ": left half looks empty; see " << out.string();
            EXPECT_GT(v2Peak, 0.02f) << pathName << ": right half looks empty; see " << out.string();

            // The discriminator. Measured: the v2 near-mirror highlight
            // saturates toward luminance ~1.0 while the Legacy collapsed lobe
            // leaves that sphere at ambient-only levels. 0.15 of headroom
            // separates "the selector reached this path's closure" from
            // tone-mapping wobble; the lane-dead regression (both spheres
            // Legacy) puts the two peaks within noise of each other and fails
            // by a wide margin.
            EXPECT_GT(v2Peak, legacyPeak + 0.15f)
                << pathName << ": the ClosureV2 sphere's specular peak (" << v2Peak
                << ") does not beat the Legacy sphere's (" << legacyPeak
                << ") — the PBRModel selector is dead on this path (see the header comment "
                << "for the exact regression this pins); evidence: " << out.string();
        }
    };

    TEST_F(ClosureV2DeferredParityScene, ModelSelectorIsLiveOnBothRasterPaths)
    {
        // Forward first (the material-UBO parameter journey) ...
        ExpectV2BeatsLegacyOnPath(RenderingPath::Forward, "Forward");
        // ... then Deferred (the G-Buffer flags-lane journey). Order matters
        // for neither correctness nor cleanup (TearDown restores settings);
        // Deferred second just makes a Deferred-only failure read last.
        ExpectV2BeatsLegacyOnPath(RenderingPath::Deferred, "Deferred");
    }
} // namespace OloEngine::Tests
