// =============================================================================
// StarNestReflectionEvidenceTest.cpp
//
// THE reflectiveness evidence for issue #292: render a metallic sphere through
// the REAL Scene pipeline (Scene::OnUpdateRuntime -> RenderScene3D -> the full
// Renderer3D render graph, including Scene::LoadAndRenderSkybox which bakes the
// StarNestSkyComponent into the shared EnvironmentMap + global IBL) and read the
// composited frame back. The sphere is Metallic=1 / low-roughness, so it
// mirror-reflects the baked nebula — i.e. this captures the exact "skybox
// reflectiveness" the issue asks for, as pixels, not just a baked cubemap.
//
// Uses the same RendererAttachedTest sized-render path as SceneRenderEvidenceTest
// (issue #258), so it runs in the normal suite when a GL 4.6 context exists and
// SKIPs cleanly otherwise.
//
// Contracts (driver-independent, deliberately loose — the PNG is the real
// artifact, pinned numerically by StarNestSkyBakeTest):
//   1. The composited frame reads back at the requested resolution.
//   2. The frame is non-trivial: a metallic sphere reflecting a starfield nebula
//      against the nebula skybox produces real luminance contrast (not a flat or
//      black frame — the regression that would mean "nothing reflected").
//   3. The frame carries colour (the nebula is not greyscale), so the reflection
//      actually picked up the baked sky rather than a flat ambient.
//
// The readback PNG is always written to
//   OloEditor/assets/tests/visual/StarNestReflection_VisualEvidence.png
//
// Classification: L8 / integration (full GL pipeline through the real Scene
// render path + StarNest bake + IBL, RGBA8 readback + PNG).
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

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "StarNestReflection_VisualEvidence.png";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // MetallicSphereUnderNebula — a metallic sphere lit only by the baked Star
    // Nest nebula's IBL, driven through OnUpdateRuntime.
    // -------------------------------------------------------------------------
    class MetallicSphereUnderNebula : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Perspective camera at +Z looking toward the origin sphere.
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // The Star Nest nebula sky. Intensity bumped so the reflection reads
            // clearly in a 256px frame. EnableIBL drives the sphere's reflection;
            // EnableSkybox paints the background behind it.
            Entity sky = GetScene().CreateEntity("Star Nest Sky");
            auto& starNest = sky.AddComponent<StarNestSkyComponent>();
            starNest.m_Intensity = 4.0f;
            starNest.m_Saturation = 0.9f;
            starNest.m_EnableSkybox = true;
            starNest.m_EnableIBL = true;
            starNest.m_CubemapResolution = 128; // keep the in-test bake fast

            // A metallic, near-mirror sphere at the origin: it reflects the
            // baked nebula (specular IBL) — this is the feature under test.
            Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 32);
            Entity sphereEntity = GetScene().CreateEntity("Metal Sphere");
            sphereEntity.AddComponent<MeshComponent>(sphere->GetMeshSource());
            auto& mat = sphereEntity.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.95f, 0.95f, 1.0f));
            mat.m_Material.SetMetallicFactor(1.0f);
            mat.m_Material.SetRoughnessFactor(0.08f);

            EnableRendering(kSize, kSize);
        }
    };

    TEST_F(MetallicSphereUnderNebula, ReflectsNebulaThroughScenePipeline)
    {
        // A few frames: the StarNest sky bakes on the first LoadAndRenderSkybox,
        // and the render graph seeds prev-frame history (TAA / velocity) before
        // producing the stable image.
        RunFrames(3);

        std::vector<u8> px;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(ReadbackComposite(px, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        EXPECT_EQ(width, kSize);
        EXPECT_EQ(height, kSize);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(width) * height * 4u);

        // Always write the PNG first, then assert.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(width), static_cast<int>(height),
                                           4, px.data(), static_cast<int>(width) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();
        OLO_CORE_INFO("StarNest reflection evidence written to {}", fs::absolute(out).string());

        // Contrast contract: a metallic sphere reflecting a starfield against the
        // nebula skybox must produce real luminance spread — a flat or black
        // frame would mean nothing reflected / the sky never baked.
        f32 minLum = 1.0f;
        f32 maxLum = 0.0f;
        f64 sumR = 0.0;
        f64 sumG = 0.0;
        f64 sumB = 0.0;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const f32 l = LuminanceAt(px, i);
            minLum = std::min(minLum, l);
            maxLum = std::max(maxLum, l);
            sumR += px[i + 0];
            sumG += px[i + 1];
            sumB += px[i + 2];
        }
        EXPECT_GT(maxLum - minLum, 0.05f)
            << "rendered frame is nearly flat (min=" << minLum << ", max=" << maxLum
            << ") — the nebula may not have baked / reflected; see " << out.string();
        EXPECT_GT(maxLum, 0.10f)
            << "rendered frame has no bright pixels — the bake/reflection output black; see "
            << out.string();

        // Colour contract: the nebula is chromatic, so the reflected frame must
        // not be greyscale (R, G, B means should differ). A grey frame would
        // mean the reflection is flat ambient, not the baked sky.
        const f64 n = static_cast<f64>(px.size() / 4);
        const f32 meanR = static_cast<f32>(sumR / n);
        const f32 meanG = static_cast<f32>(sumG / n);
        const f32 meanB = static_cast<f32>(sumB / n);
        const f32 chroma = std::max({ std::abs(meanR - meanG), std::abs(meanG - meanB),
                                      std::abs(meanR - meanB) });
        EXPECT_GT(chroma, 1.0f)
            << "frame is essentially greyscale (R=" << meanR << " G=" << meanG << " B=" << meanB
            << ") — the reflection did not pick up the chromatic nebula; see " << out.string();
    }
} // namespace OloEngine::Tests
