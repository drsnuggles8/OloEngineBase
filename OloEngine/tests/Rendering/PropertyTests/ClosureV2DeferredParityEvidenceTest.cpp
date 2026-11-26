// OLO_TEST_LAYER: L8
// Forward and deferred must both render the sharp silver highlight beside its dark control.
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
            // Black metal is the dark control for the bright mirror.
            {
                Entity entity = GetScene().CreateEntity("DarkMetal");
                entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
                entity.GetComponent<TransformComponent>().Translation = { -1.4f, 0.0f, 0.0f };
                auto& materialComp = entity.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(1.0f);
                materialComp.m_Material.SetRoughnessFactor(0.02f);
            }
            // Same roughness and metalness, with a silver reflectance.
            {
                Entity entity = GetScene().CreateEntity("ClosureV2Mirror");
                entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
                entity.GetComponent<TransformComponent>().Translation = { 1.4f, 0.0f, 0.0f };
                auto& materialComp = entity.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.93f, 0.9f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(1.0f);
                materialComp.m_Material.SetRoughnessFactor(0.02f);
            }

            EnableRendering(kSize, kSize);
        }

        void ExpectMirrorHighlight(RenderingPath path, const char* pathName)
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

            const f32 darkPeak = PeakLuminanceInHalf(px, width, height, /*rightHalf=*/false);
            const f32 v2Peak = PeakLuminanceInHalf(px, width, height, /*rightHalf=*/true);

            EXPECT_GT(v2Peak, 0.75f) << pathName << ": missing near-mirror highlight in " << out.string();
            EXPECT_GT(v2Peak, darkPeak + 0.15f) << pathName << ": mirror and dark control peaks " << v2Peak << ", " << darkPeak;
        }
    };

    TEST_F(ClosureV2DeferredParityScene, NearMirrorMetalRetainsItsPeakOnBothRasterPaths)
    {
        ExpectMirrorHighlight(RenderingPath::Forward, "Forward");
        ExpectMirrorHighlight(RenderingPath::Deferred, "Deferred");
    }
} // namespace OloEngine::Tests
