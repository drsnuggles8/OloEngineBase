// OLO_TEST_LAYER: integration
// =============================================================================
// TransparentBlendOrderVisualEvidenceTest.cpp
//
// Real-pixel evidence for issue #1327 — conventional alpha-blended draws must
// composite back-to-front across DIFFERENT materials, rather than in the order
// their shader and material IDs happen to fall.
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
// All three rendering paths carry the same positive assertion. The Deferred
// cell used to be a tripwire: a blended classic mesh was written into the
// G-Buffer with its blend state applied to the G-Buffer channels and shaded to
// pure black (issue #1404). It is now rerouted to ForwardOverlayPass, which
// shades it forward over the lit deferred image and sorts it with the same
// depth-major transparent key, so the ordering claim holds there too. The
// Deferred cell is run with and without G-Buffer MSAA, since the overlay
// composites into the scene framebuffer after the MSAA resolve.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
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
        // Behind both, for the batch-candidate slab (see BuildScene).
        constexpr f32 kBackZ = -3.0f;

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

            // No editor helpers. A shipped game has none, and on the Deferred
            // path the grid and gizmos are ForwardOverlayPass draws of their
            // own: with them present, a blended mesh is never the first draw
            // in that pass. With them absent it is, which is the case that
            // crashed the editor during #1404's live check.
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);
            scene.SetLightGizmosVisible(false);
            scene.SetCameraFrustumsVisible(false);

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

            if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                m_SharedCube = mesh->GetMeshSource();

            // Opaque black backdrop. The worked example composites over black,
            // and without it the clear colour (and, on the deferred path, the
            // sky) would contribute to the centre pixels.
            //
            // It shares the slabs' MeshSource, and so their vertex array, on
            // purpose. On the Deferred path the backdrop is the last G-Buffer
            // draw and a slab is the first ForwardOverlayPass draw, with the
            // fullscreen lighting passes, which unbind the VAO directly, in
            // between. A dispatcher bind cache that survives that gap skips the
            // slab's VAO bind and draws from VAO 0, which is a driver access
            // violation. That is how every primitive cube in an editor scene is
            // set up, and it is what crashed the editor during #1404's check.
            {
                Entity backdrop = scene.CreateEntity("Backdrop");
                auto& tc = backdrop.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 0.0f, -6.0f };
                tc.Scale = { 40.0f, 30.0f, 0.2f };
                auto& mc = backdrop.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                mc.m_MeshSource = m_SharedCube;
                auto& mat = backdrop.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetRoughnessFactor(1.0f);
                mat.m_Material.SetMetallicFactor(0.0f);
            }

            m_Red = AddBlendedQuad("RedQuad", glm::vec3(1.0f, 0.0f, 0.0f), kNearZ, m_RedAlbedo);
            m_Blue = AddBlendedQuad("BlueQuad", glm::vec3(0.0f, 0.0f, 1.0f), kFarZ, m_BlueAlbedo);

            // A THIRD slab, behind the other two, carrying byte-identical
            // material content to m_Red (same base colour, same albedo texture
            // object) and the same MeshSource.
            //
            // This one exists solely so the batching cell is not vacuous.
            // FrameDataBuffer::AllocateMaterialData dedupes on content, so
            // m_Red and m_RedBack resolve to the SAME materialDataIndex, the
            // same vertexArrayID and the same renderStateIndex — i.e. they are
            // a real instance-group candidate, and the only thing keeping them
            // apart is the depth-major blendOrderKey this PR adds. m_Red and
            // m_Blue deliberately differ in material and could never have
            // grouped, so a batching comparison built on that pair proves
            // nothing.
            m_RedBack = AddBlendedQuad("RedQuadBack", glm::vec3(1.0f, 0.0f, 0.0f), kBackZ, m_RedAlbedo);
        }

        // `albedoMap` is passed in rather than created per quad: two quads that
        // are meant to be batch candidates must share the texture OBJECT, since
        // the material POD carries its RHI handle.
        Entity AddBlendedQuad(const char* name, const glm::vec3& albedo, f32 z, Ref<Texture2D>& albedoMap)
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
            mc.m_MeshSource = m_SharedCube;

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
            if (!albedoMap)
                albedoMap = CreateWhitePixel();
            material.SetAlbedoMap(albedoMap);
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

        // An instance's transform is world space, so an instanced slab carries
        // its placement in its single InstanceData rather than in the entity's
        // TransformComponent.
        [[nodiscard]] static glm::mat4 SlabTransform(f32 z)
        {
            return glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z)), glm::vec3(6.0f, 6.0f, 0.02f));
        }

        void SetSlabZ(Entity slab, f32 z)
        {
            slab.GetComponent<TransformComponent>().Translation.z = z;
            if (slab.HasComponent<InstancedMeshComponent>())
            {
                auto& imc = slab.GetComponent<InstancedMeshComponent>();
                imc.Instances[0].Transform = SlabTransform(z);
                imc.Instances[0].PrevTransform = imc.Instances[0].Transform;
                imc.InvalidateMergedCache();
            }
        }

        // Re-submit the three blended slabs through Renderer3D::DrawMeshInstanced
        // (an InstancedMeshComponent of one instance each) instead of DrawMesh,
        // keeping their MaterialComponent, which the instanced path reads as its
        // override. The backdrop stays a MeshComponent.
        void ConvertSlabsToInstanced()
        {
            for (Entity slab : { m_Red, m_Blue, m_RedBack })
            {
                const f32 z = slab.GetComponent<TransformComponent>().Translation.z;
                slab.RemoveComponent<MeshComponent>();
                auto& tc = slab.GetComponent<TransformComponent>();
                tc.Scale = glm::vec3(1.0f);
                auto& imc = slab.AddComponent<InstancedMeshComponent>();
                imc.MeshSource = m_SharedCube;
                imc.CastShadows = false;
                InstanceData instance;
                instance.Transform = SlabTransform(z);
                instance.PrevTransform = instance.Transform;
                imc.Instances.Add(instance);
            }
        }

        // Place `nearEntity` in front and the other behind, render, and return
        // the mean centre colour.
        Rgb CaptureWithNear(const std::string& label, Entity nearEntity, Entity farEntity)
        {
            SetSlabZ(nearEntity, kNearZ);
            SetSlabZ(farEntity, kFarZ);
            std::vector<u8> pixels;
            Capture(label, pixels);
            if (::testing::Test::HasFatalFailure())
                return Rgb{};
            return MeanCenterColor(pixels);
        }

        // The whole claim, on one rendering path.
        void RunPath(const char* pathName, RenderingPath path, u32 deferredMsaaSamples = 1u)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            const u32 savedSamples = settings.Deferred.MSAASampleCount;
            settings.Path = path;
            settings.Deferred.MSAASampleCount = deferredMsaaSamples;
            Renderer3D::ApplyRendererSettings();
            RunPathWithCurrentSettings(pathName);
            settings.Deferred.MSAASampleCount = savedSamples;
            Renderer3D::ApplyRendererSettings();
        }

        void RunPathWithCurrentSettings(const char* pathName)
        {

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
        Entity m_RedBack;

        // Shared so the batch-candidate pair really is one: the instance group
        // key is (vertexArrayID, indexCount, baseIndex, materialDataIndex,
        // renderStateIndex, …), and the material POD carries the albedo map's
        // RHI handle.
        Ref<MeshSource> m_SharedCube;
        Ref<Texture2D> m_RedAlbedo;
        Ref<Texture2D> m_BlueAlbedo;
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

    // Issue #1404: before the fix both Deferred captures read exactly
    // (0, 0, 0) over the slabs, because the blended draws went into the
    // G-Buffer. The "frame is black" precondition inside RunPath is what that
    // defect trips; the dominance pair then proves the overlay route kept
    // #1327's depth-major order.
    TEST_F(TransparentBlendOrderVisualEvidence, DeferredBlendsTheNearerQuadLast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunPath("Deferred", RenderingPath::Deferred);
    }

    TEST_F(TransparentBlendOrderVisualEvidence, DeferredMsaaBlendsTheNearerQuadLast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunPath("DeferredMSAA4", RenderingPath::Deferred, 4u);
    }

    // The same claim through the INSTANCED submission route
    // (Renderer3D::DrawMeshInstanced -> SelectInstancedShaderRouting), which
    // makes its own shader choice and its own overlay decision. Forward is the
    // control: it shows the instanced fixture itself composites correctly, so a
    // Deferred failure is the route and not the fixture.
    TEST_F(TransparentBlendOrderVisualEvidence, ForwardInstancedBlendsTheNearerQuadLast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ConvertSlabsToInstanced();
        RunPath("ForwardInstanced", RenderingPath::Forward);
    }

    TEST_F(TransparentBlendOrderVisualEvidence, DeferredInstancedBlendsTheNearerQuadLast)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ConvertSlabsToInstanced();
        RunPath("DeferredInstanced", RenderingPath::Deferred);
    }

    TEST_F(TransparentBlendOrderVisualEvidence, BatchingDisabledProducesTheSameImage)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        // Criterion 2's "batching on/off" cell, on real pixels.
        //
        // The subject is m_Red and m_RedBack, NOT m_Red and m_Blue: the batcher
        // groups on (vertexArrayID, indexCount, baseIndex, materialDataIndex,
        // renderStateIndex, …), and m_Red/m_Blue differ in material, so they
        // could never have grouped and a comparison built on them would pass
        // whatever the batcher did. m_Red and m_RedBack share a MeshSource and
        // byte-identical material content, so they ARE a candidate group — and
        // the blue slab sits between them in depth. If the batcher collapsed
        // them onto the nearer one's key, the blue would stop compositing
        // between them and the image would move.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        auto* geometry = Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry);
        ASSERT_TRUE(geometry) << "no geometry render-stream node to toggle the batcher on";
        CommandBucket& bucket = geometry->GetCommandBucket();
        const CommandBucketConfig savedConfig = bucket.GetConfig();

        // Precondition: the pair really is batchable apart from depth. If this
        // ever stops holding, the comparison below becomes vacuous and this
        // says so instead of passing.
        const Material& redNear = m_Red.GetComponent<MaterialComponent>().m_Material;
        const Material& redBack = m_RedBack.GetComponent<MaterialComponent>().m_Material;
        ASSERT_EQ(ComputeMaterialID(redNear), ComputeMaterialID(redBack))
            << "the two red slabs no longer share a material, so nothing here can batch";
        ASSERT_EQ(m_Red.GetComponent<MeshComponent>().m_MeshSource.Raw(),
                  m_RedBack.GetComponent<MeshComponent>().m_MeshSource.Raw())
            << "the two red slabs no longer share a mesh, so nothing here can batch";

        CommandBucketConfig config = savedConfig;
        config.EnableBatching = true;
        bucket.SetConfig(config);
        // `BatchedCommands` is cumulative for the bucket's lifetime — `Clear()`
        // (the per-frame path) does not reset it, only `Reset(allocator)` does —
        // and this bucket is process-global, so by the time this test runs it
        // already carries whatever earlier tests in the process batched. The
        // delta across the batching-ON window is the only honest reading.
        const u32 batchedBefore = bucket.GetStatistics().BatchedCommands;
        const Rgb batched = CaptureWithNear("Forward_BatchingOn", m_Red, m_Blue);
        const u32 batchedDuring = bucket.GetStatistics().BatchedCommands - batchedBefore;

        config.EnableBatching = false;
        bucket.SetConfig(config);
        const Rgb unbatched = CaptureWithNear("Forward_BatchingOff", m_Red, m_Blue);

        bucket.SetConfig(savedConfig);
        if (::testing::Test::HasFatalFailure())
            return;

        // The guarantee: two blended draws at different depths are never
        // collapsed, however batchable they otherwise are. The scene's only
        // opaque mesh is the backdrop, which is alone in its group, so any
        // collapse counted here is the red pair.
        EXPECT_EQ(batchedDuring, 0u)
            << "auto-batching collapsed " << batchedDuring
            << " source draw(s) while rendering this scene. The only group candidate here is the "
               "red pair, which sits at two different depths — the #1327 guarantee is exactly "
               "that it must not be collapsed.";

        EXPECT_GT(batched.R, batched.B) << "batched: red is nearer and must dominate";
        EXPECT_GT(unbatched.R, unbatched.B) << "unbatched: red is nearer and must dominate";
        EXPECT_NEAR(batched.R, unbatched.R, 2.0) << "auto-batching changed the composited red channel";
        EXPECT_NEAR(batched.B, unbatched.B, 2.0) << "auto-batching changed the composited blue channel";
    }
} // namespace OloEngine::Tests
