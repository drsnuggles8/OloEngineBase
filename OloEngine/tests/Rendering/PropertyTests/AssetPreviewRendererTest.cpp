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
// We deliberately do NOT assert anything about pixel contents — the math
// in `MaterialPreview.glsl` is exercised separately by the PBR property
// tests (Fresnel, NDF, geometry), which is the right granularity. This
// fixture's job is to catch "did we wire the pipeline back together at
// all" regressions: shader path drift, framebuffer attachment mismatches,
// texture-format incompatibilities in the FB → Texture2D copy step, etc.
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

namespace OloEngine::Tests
{
    namespace
    {
        // The renderer can't run without a basic Renderer3D so that
        // `MaterialAsset` can resolve its DefaultPBR shader from the
        // shader library. We don't need the whole pipeline up — just
        // a compiled `DefaultPBR` so the MaterialAsset constructor
        // succeeds. Static so this initialisation happens once per
        // process.
        class RendererBoot
        {
          public:
            static void EnsureMinimalShaderLibrary()
            {
                ShaderLibrary& lib = Renderer3D::GetShaderLibrary();
                if (!lib.Exists("DefaultPBR"))
                {
                    auto shader = Shader::Create("assets/shaders/PBR_MultiLight.glsl");
                    if (shader)
                        lib.Add("DefaultPBR", shader);
                }
            }
        };

        // Build a stock MaterialAsset and tweak its factors a little so
        // the test doesn't fail spuriously when default values change.
        Ref<MaterialAsset> MakeTestMaterial()
        {
            RendererBoot::EnsureMinimalShaderLibrary();
            auto material = Ref<MaterialAsset>::Create(false);
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

        Ref<Texture2D> thumbnail = AssetPreviewRenderer::RenderMaterialPreview(material);
        ASSERT_TRUE(thumbnail);
        EXPECT_EQ(thumbnail->GetWidth(), AssetPreviewRenderer::kThumbnailSize);
        EXPECT_EQ(thumbnail->GetHeight(), AssetPreviewRenderer::kThumbnailSize);
        EXPECT_NE(thumbnail->GetRendererID(), 0u);
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
