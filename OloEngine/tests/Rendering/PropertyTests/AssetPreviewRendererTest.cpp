// =============================================================================
// AssetPreviewRendererTest.cpp
//
// Smoke tests for `AssetPreviewRenderer` — the offscreen renderer that
// produces Content Browser thumbnails for Material / Mesh assets. We
// verify that:
//
//   - Initialize() with the standard shader path succeeds on a GL 4.6
//     context (the shader compiles, the framebuffer + sphere mesh are
//     ready).
//   - RenderMaterialPreview() against a stock `MaterialAsset` produces a
//     non-null `Texture2D` of the documented `kThumbnailSize` dimensions.
//   - RenderMeshPreview() with no material falls back to the neutral
//     default and produces a similarly-sized texture.
//   - Shutdown is idempotent.
//
// The material thumbnail is read back at two roughnesses to verify that the
// shared closure reaches the content-browser texture through its preview rig.
//
// Skips when no GL context is available (CI without a GPU).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Preview/AssetPreviewRenderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Texture.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The material is built from a shader this test loads itself rather
        // than one registered in Renderer3D's shader library: the library
        // outlives the test, and a later Renderer3D::Init adding the same
        // name asserts. Not a static: a Ref held past the test would be
        // destroyed at process exit, after the GL context.
        Ref<Shader> PreviewShader()
        {
            return Shader::Create("assets/shaders/PBR_MultiLight.glsl");
        }

        // Build a stock MaterialAsset and tweak its factors a little so
        // the test doesn't fail spuriously when default values change.
        Ref<MaterialAsset> MakeTestMaterial()
        {
            auto material = Ref<MaterialAsset>::Create(Material::Create(PreviewShader()));
            material->SetAlbedoColor(glm::vec3(0.6f, 0.2f, 0.2f));
            material->SetMetalness(0.3f);
            material->SetRoughness(0.5f);
            material->SetEmission(0.0f);
            return material;
        }

        // Tears the renderer down between tests so we exercise both the
        // first-time and warm-cache paths.
        struct RendererScope
        {
            ~RendererScope()
            {
                AssetPreviewRenderer::Shutdown();
            }
        };
    } // namespace

    TEST(AssetPreviewRendererTest, InitializeAndShutdownAreIdempotent)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RendererScope guard;
        AssetPreviewRenderer::Initialize();
        EXPECT_TRUE(AssetPreviewRenderer::IsInitialized());
        // Calling Initialize again with the renderer already up should
        // be a no-op — the cached FB / shader stay intact.
        AssetPreviewRenderer::Initialize();
        EXPECT_TRUE(AssetPreviewRenderer::IsInitialized());

        AssetPreviewRenderer::Shutdown();
        EXPECT_FALSE(AssetPreviewRenderer::IsInitialized());
        // Second Shutdown must be tolerated too — releases nothing.
        AssetPreviewRenderer::Shutdown();
        EXPECT_FALSE(AssetPreviewRenderer::IsInitialized());
    }

    TEST(AssetPreviewRendererTest, RenderMaterialPreviewReturnsCorrectlySizedTexture)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RendererScope guard;
        AssetPreviewRenderer::Initialize();
        ASSERT_TRUE(AssetPreviewRenderer::IsInitialized());

        Ref<MaterialAsset> material = MakeTestMaterial();
        ASSERT_TRUE(material);

        constexpr u32 size = AssetPreviewRenderer::kThumbnailSize;
        const auto directory = std::filesystem::path("assets/tests/visual");
        std::filesystem::create_directories(directory);
        const auto capture = [&](f32 roughness, const char* name, std::vector<u8>& pixels)
        {
            material->SetRoughness(roughness);
            const Ref<Texture2D> thumbnail = AssetPreviewRenderer::RenderMaterialPreview(material);
            ASSERT_TRUE(thumbnail);
            ASSERT_EQ(thumbnail->GetWidth(), size);
            ASSERT_EQ(thumbnail->GetHeight(), size);
            EXPECT_NE(thumbnail->GetRendererID(), 0u);
            ReadbackRgba8(thumbnail->GetRHIHandle(), size, size, pixels);
            ASSERT_EQ(pixels.size(), size * size * 4u);
            ASSERT_NE(stbi_write_png((directory / name).string().c_str(), size, size, 4, pixels.data(), size * 4), 0);
        };

        std::vector<u8> smooth;
        std::vector<u8> rough;
        ASSERT_NO_FATAL_FAILURE(capture(0.12f, "MaterialPreviewSmooth_OpenGL_Thumbnail.png", smooth));
        ASSERT_NO_FATAL_FAILURE(capture(0.85f, "MaterialPreviewRough_OpenGL_Thumbnail.png", rough));
        u32 changed = 0;
        for (sizet i = 0; i < smooth.size(); i += 4)
        {
            if (std::abs(static_cast<int>(smooth[i]) - rough[i]) >= 4)
                ++changed;
        }
        RecordProperty("ChangedRedPixels", changed);
        EXPECT_GT(changed, size * size / 100u) << "material roughness must change the rendered thumbnail";
    }

    TEST(AssetPreviewRendererTest, RenderMaterialPreviewWithNullMaterialReturnsNull)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RendererScope guard;
        AssetPreviewRenderer::Initialize();
        ASSERT_TRUE(AssetPreviewRenderer::IsInitialized());

        Ref<Texture2D> thumbnail = AssetPreviewRenderer::RenderMaterialPreview(nullptr);
        EXPECT_FALSE(thumbnail);
    }

    TEST(AssetPreviewRendererTest, RenderMeshPreviewWithoutMaterialUsesDefaultAndProducesTexture)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RendererScope guard;
        AssetPreviewRenderer::Initialize();
        ASSERT_TRUE(AssetPreviewRenderer::IsInitialized());

        // Use a fresh icosphere so the mesh path goes through the AABB
        // framing code instead of short-circuiting on the cached sphere
        // (which uses a fixed camera). Any valid mesh is fine here.
        Ref<Mesh> mesh = MeshPrimitives::CreateIcosphere(1.0f, 1);
        ASSERT_TRUE(mesh);

        Ref<Texture2D> thumbnail = AssetPreviewRenderer::RenderMeshPreview(mesh, nullptr);
        ASSERT_TRUE(thumbnail);
        EXPECT_EQ(thumbnail->GetWidth(), AssetPreviewRenderer::kThumbnailSize);
        EXPECT_EQ(thumbnail->GetHeight(), AssetPreviewRenderer::kThumbnailSize);
    }

    TEST(AssetPreviewRendererTest, RenderingBeforeInitializeReturnsNullWithoutCrashing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Ensure renderer is *not* up.
        AssetPreviewRenderer::Shutdown();
        ASSERT_FALSE(AssetPreviewRenderer::IsInitialized());

        Ref<MaterialAsset> material = MakeTestMaterial();
        ASSERT_TRUE(material);
        EXPECT_FALSE(AssetPreviewRenderer::RenderMaterialPreview(material));
        EXPECT_FALSE(AssetPreviewRenderer::RenderMeshPreview(nullptr, nullptr));
    }
} // namespace OloEngine::Tests
