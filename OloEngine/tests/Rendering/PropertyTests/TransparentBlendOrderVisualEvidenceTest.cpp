// OLO_TEST_LAYER: integration
// =============================================================================
// TransparentBlendOrderVisualEvidenceTest.cpp
//
// Real-pixel evidence for issue #1327 — conventional alpha-blended draws must
// composite back-to-front across DIFFERENT materials, not in material-ID order.
//
// The scene is the issue's worked example made renderable: a black backdrop,
// a 50%-alpha RED quad and a 50%-alpha BLUE quad, both filling the centre of
// the frame, one in front of the other. Two captures per rendering path swap
// which quad is nearer:
//
//   NearRed  — red in front  → the composite must read RED-dominant
//   NearBlue — blue in front → the composite must read BLUE-dominant
//
// Why the pair and not a single absolute colour: the frame is tone-mapped and
// sRGB-encoded, so (0.5, 0, 0.25) linear is not (128, 0, 64) on screen. What
// survives any monotone transfer is WHICH quad is on top, and the pair is what
// makes that a real discriminator. Before the fix the draw order was decided by
// the two materials' 16-bit ID hashes, which do not change when the quads swap
// places — so BOTH captures came out with the same quad on top and one of them
// was simply wrong. A single capture could have passed by luck.
//
// The two materials must therefore have DIFFERENT sort material IDs, which is
// why each carries its own 1x1 white albedo texture (ComputeMaterialID hashes
// texture identities; two untextured PBR materials hash to the same ID and the
// defect is unreachable). That is asserted as a precondition, not assumed.
//
// The CPU-side ordering contract — the sort key, batching, parallel submission,
// tie determinism — lives in Rendering/TransparentDepthOrderingTest.cpp. This
// file only answers "and is the framebuffer right?".
//
// Forward and Forward+ carry the positive assertion. Deferred cannot yet: a
// blended classic mesh shades to pure black there for a reason that has
// nothing to do with ordering (issue #1404), so that cell is a tripwire on the
// defect instead — see DeferredBlendedMeshesStillRenderBlack at the bottom.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;

        // Both quads sit at the origin, separated along Z; the camera looks
        // down -Z from +Z, so a smaller Z is FARTHER from the camera.
        constexpr f32 kNearZ = 1.0f;
        constexpr f32 kFarZ = -1.0f;

        struct Rgb
        {
            f64 R = 0.0, G = 0.0, B = 0.0;
        };

        // Mean colour over the centre 20% of the frame, where both quads cover
        // every pixel. Row 0 is the top (the caller has already flipped).
        [[nodiscard]] Rgb MeanCenterColor(const std::vector<u8>& rgba)
        {
            const u32 x0 = kWidth * 2u / 5u;
            const u32 x1 = kWidth * 3u / 5u;
            const u32 y0 = kHeight * 2u / 5u;
            const u32 y1 = kHeight * 3u / 5u;
            Rgb sum;
            u32 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    sum.R += rgba[i + 0];
                    sum.G += rgba[i + 1];
                    sum.B += rgba[i + 2];
                    ++count;
                }
            }
            if (count == 0)
                return sum;
            return Rgb{ sum.R / count, sum.G / count, sum.B / count };
        }

        // A 1x1 white texture. White so it does not tint the base colour; a
        // distinct engine texture per material so the two materials get
        // distinct 16-bit sort IDs (see ComputeMaterialID).
        [[nodiscard]] Ref<Texture2D> CreateWhitePixel()
        {
            TextureSpecification spec;
            spec.Width = 1;
            spec.Height = 1;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            Ref<Texture2D> texture = Texture2D::Create(spec);
            if (texture)
            {
                u32 white = 0xFFFFFFFFu;
                texture->SetData(&white, sizeof(white));
            }
            return texture;
        }
    } // namespace

    class TransparentBlendOrderVisualEvidence : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 4.0f, 6.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Straight down the view axis: both quads face the camera, so
                // each one's shaded colour is its own albedo and nothing about
                // the lighting can favour one over the other.
                dl.m_Direction = glm::normalize(glm::vec3(0.0f, 0.0f, -1.0f));
                dl.m_Color = glm::vec3(1.0f);
                dl.m_Intensity = 3.0f;
            }

            // Opaque black backdrop. The worked example composites over black,
            // and without it the clear colour (and, on the deferred path, the
            // sky) would contribute to the centre pixels.
            {
                Entity backdrop = scene.CreateEntity("Backdrop");
                auto& tc = backdrop.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, -6.0f };
                tc.Scale = { 40.0f, 30.0f, 0.2f };
                auto& mc = backdrop.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = backdrop.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetRoughnessFactor(1.0f);
                mat.m_Material.SetMetallicFactor(0.0f);
            }

            m_Red = AddBlendedQuad("RedQuad", glm::vec3(1.0f, 0.0f, 0.0f), kNearZ);
            m_Blue = AddBlendedQuad("BlueQuad", glm::vec3(0.0f, 0.0f, 1.0f), kFarZ);
        }

        Entity AddBlendedQuad(const char* name, const glm::vec3& albedo, f32 z)
        {
            Scene& scene = GetScene();
            Entity e = scene.CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = { 0.0f, 0.0f, z };
            // Flat slabs, generously oversized so the centre crop is covered by
            // both of them at every camera pose used here.
            tc.Scale = { 6.0f, 6.0f, 0.02f };
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                mc.m_MeshSource = mesh->GetMeshSource();

            auto& mat = e.AddComponent<MaterialComponent>();
            Material& material = mat.m_Material;
            material.SetBaseColorFactor(glm::vec4(albedo, 0.5f));
            material.SetRoughnessFactor(1.0f);
            material.SetMetallicFactor(0.0f);
            material.SetAlphaMode(AlphaMode::Blend);
            material.SetFlag(MaterialFlag::Blend, true);
            // Two-sided so the slab shades identically whichever side the
            // camera ends up on, and so a back-face cull cannot silently drop
            // one of the two subjects.
            material.SetFlag(MaterialFlag::TwoSided, true);
            material.SetAlbedoMap(CreateWhitePixel());
            return e;
        }

        // Render the current scene state and read back the composite, top-row
        // first, writing the PNG evidence next to the other visual captures.
        void Capture(const std::string& label, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 0.0f, 8.0f), 0.0f, 0.0f);

            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for " << label;

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL returns rows bottom-up; the PNG writer treats row 0 as the top.
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
            const std::string path = (dir / ("TransparentBlendOrder_" + label + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, outPixels.data(), static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        // Place `nearEntity` in front and the other behind, render, and return
        // the mean centre colour.
        Rgb CaptureWithNear(const std::string& label, Entity nearEntity, Entity farEntity)
        {
            nearEntity.GetComponent<TransformComponent>().Translation.z = kNearZ;
            farEntity.GetComponent<TransformComponent>().Translation.z = kFarZ;
            std::vector<u8> pixels;
            Capture(label, pixels);
            if (::testing::Test::HasFatalFailure())
                return Rgb{};
            return MeanCenterColor(pixels);
        }

        // The whole claim, on one rendering path.
        void RunPath(const char* pathName, RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Precondition: the two materials must actually differ in the sort
            // key, or the defect this test exists for is unreachable and every
            // assertion below would pass vacuously.
            const u32 redMaterialID = ComputeMaterialID(m_Red.GetComponent<MaterialComponent>().m_Material);
            const u32 blueMaterialID = ComputeMaterialID(m_Blue.GetComponent<MaterialComponent>().m_Material);
            ASSERT_NE(redMaterialID, blueMaterialID)
                << pathName << ": both quads hash to sort material ID " << redMaterialID
                << ", so material-major ordering and depth-major ordering are indistinguishable here.";

            const Rgb nearRed = CaptureWithNear(std::string(pathName) + "_NearRed", m_Red, m_Blue);
            if (::testing::Test::HasFatalFailure())
                return;
            const Rgb nearBlue = CaptureWithNear(std::string(pathName) + "_NearBlue", m_Blue, m_Red);
            if (::testing::Test::HasFatalFailure())
                return;

            auto describe = [&](const char* which, const Rgb& c)
            {
                return std::string(pathName) + " " + which + " mean centre RGB = (" +
                       std::to_string(c.R) + ", " + std::to_string(c.G) + ", " + std::to_string(c.B) +
                       ")  [red material ID " + std::to_string(redMaterialID) +
                       ", blue material ID " + std::to_string(blueMaterialID) + "]";
            };

            // Precondition: something was actually drawn.
            ASSERT_GT(nearRed.R + nearRed.G + nearRed.B, 6.0) << describe("NearRed", nearRed) << " — frame is black";
            ASSERT_GT(nearBlue.R + nearBlue.G + nearBlue.B, 6.0) << describe("NearBlue", nearBlue) << " — frame is black";

            // The claim: the quad in FRONT dominates, both ways round.
            EXPECT_GT(nearRed.R, nearRed.B)
                << describe("NearRed", nearRed)
                << "\nRed is nearer, so it blends LAST and must dominate. Blue dominating means the sort put "
                   "the far quad on top — the #1327 defect.";
            EXPECT_GT(nearBlue.B, nearBlue.R)
                << describe("NearBlue", nearBlue)
                << "\nBlue is nearer, so it blends LAST and must dominate.";

            // And the pair really is a discriminator: swapping the depths must
            // swap the dominance. A material-major sort produces the SAME
            // dominance in both captures, which is exactly what this catches.
            EXPECT_GT(nearRed.R - nearRed.B, 4.0)
                << describe("NearRed", nearRed) << " — dominance margin too small to be evidence";
            EXPECT_GT(nearBlue.B - nearBlue.R, 4.0)
                << describe("NearBlue", nearBlue) << " — dominance margin too small to be evidence";
        }

        Entity m_Red;
        Entity m_Blue;
    };

    TEST_F(TransparentBlendOrderVisualEvidence, ForwardBlendsTheNearerQuadLast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunPath("Forward", RenderingPath::Forward);
    }

    TEST_F(TransparentBlendOrderVisualEvidence, ForwardPlusBlendsTheNearerQuadLast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunPath("ForwardPlus", RenderingPath::ForwardPlus);
    }

    // The deferred cell of criterion 2 — as a TRIPWIRE, because the pixel
    // assertion cannot be made there yet.
    //
    // An alpha-blended classic mesh renders pure black on RenderingPath::Deferred
    // (issue #1404). `Renderer3D::DrawMesh` reroutes only TRANSMISSIVE PBR
    // materials to ForwardOverlayPass; a merely blended one goes to
    // `PBRGBufferShader`, so SRC_ALPHA/ONE_MINUS_SRC_ALPHA is applied to the
    // G-Buffer's albedo, normal and packed-flags channels and the lighting pass
    // reads the result. That is a SHADING defect, not an ordering one: it
    // reproduces byte-identically on master @ 6d29f8269 without the #1327
    // change, and the two slabs' material IDs differ from run to run while the
    // output stays exactly zero.
    //
    // Asserting the defect rather than skipping the cell means this test fails
    // the moment #1404 is fixed, which is when the real assertion below it
    // should be switched on. A GTEST_SKIP would go on passing forever.
    TEST_F(TransparentBlendOrderVisualEvidence, DeferredBlendedMeshesStillRenderBlack)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        const Rgb nearRed = CaptureWithNear("Deferred_NearRed", m_Red, m_Blue);
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_LT(nearRed.R + nearRed.G + nearRed.B, 1.0)
            << "Blended meshes are no longer black on the deferred path — mean centre RGB = ("
            << nearRed.R << ", " << nearRed.G << ", " << nearRed.B << ").\n"
            << "Issue #1404 appears fixed. Delete this tripwire and replace it with:\n"
            << "    RunPath(\"Deferred\", RenderingPath::Deferred);\n"
            << "which is the assertion the other two paths already carry.";
    }

    TEST_F(TransparentBlendOrderVisualEvidence, BatchingDisabledProducesTheSameImage)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // Criterion 2's "batching on/off" cell, on real pixels: the two quads
        // share a mesh and differ only in material, so they are adjacent in the
        // batcher's candidate set. Turning auto-batching off must not move the
        // image.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        auto* geometry = Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry);
        ASSERT_TRUE(geometry) << "no geometry render-stream node to toggle the batcher on";
        CommandBucket& bucket = geometry->GetCommandBucket();
        const CommandBucketConfig savedConfig = bucket.GetConfig();

        CommandBucketConfig config = savedConfig;
        config.EnableBatching = true;
        bucket.SetConfig(config);
        const Rgb batched = CaptureWithNear("Forward_BatchingOn", m_Red, m_Blue);

        config.EnableBatching = false;
        bucket.SetConfig(config);
        const Rgb unbatched = CaptureWithNear("Forward_BatchingOff", m_Red, m_Blue);

        bucket.SetConfig(savedConfig);
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_GT(batched.R, batched.B) << "batched: red is nearer and must dominate";
        EXPECT_GT(unbatched.R, unbatched.B) << "unbatched: red is nearer and must dominate";
        EXPECT_NEAR(batched.R, unbatched.R, 2.0) << "auto-batching changed the composited red channel";
        EXPECT_NEAR(batched.B, unbatched.B, 2.0) << "auto-batching changed the composited blue channel";
    }
} // namespace OloEngine::Tests
