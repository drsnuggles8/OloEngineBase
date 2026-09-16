// OLO_TEST_LAYER: L8
// Full production raster pipeline: wind on/off, motion and pause across GL paths.
#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"
#include "RendererStateCheck.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <stb_image/stb_image_write.h>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // The Drift conifer, relative to OloEditor/ (the suite's working
        // directory) — the same path Woodland.olo and the impostor bake use.
        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        // The hand-over band this test authors. Wide enough that the near camera
        // sits well inside it and the far camera well outside, so neither
        // assertion depends on where exactly the dither lands.
        constexpr f32 kMeshFadeStart = 55.0f;
        constexpr f32 kMeshViewDistance = 70.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

    } // namespace

    class FoliageWindEvidenceTest : public RendererAttachedTest
    {
      protected:
        RendererState::Snapshot m_SavedState;

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            RendererState::Restore(m_SavedState);
        }

        void BuildScene() override
        {
            ASSERT_TRUE(RendererState::Capture(m_SavedState));
            auto& wind = Renderer3D::GetWindSettings();
            wind = WindSettings{};
            wind.Enabled = true;
            wind.Direction = glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f));
            wind.Speed = 8.0f;
            wind.GustStrength = 0.6f;
            wind.GustFrequency = 0.4f;
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            m_TerrainEntity = scene.CreateEntityWithUUID(UUID(1236), "Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 11;
                terrain.m_ProceduralResolution = 128;
                terrain.m_ProceduralOctaves = 4;
                terrain.m_ProceduralFrequency = 1.5f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                // Low relief: the plants, not the hillside, have to be what
                // changes between the two arms.
                terrain.m_HeightScale = 6.0f;
                terrain.m_TessellationEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (auto layer : TerrainGenerator::MakeDefaultLayers())
                {
                    layer.BaseColor = glm::vec3(0.4f); // neutral terrain cannot satisfy the green plant mask
                    terrain.m_Material->AddLayer(layer);
                }

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;

                FoliageLayer pines;
                pines.Name = "Pines";
                pines.MeshPath = kPineMesh;
                pines.AlbedoPath = kFoliageAlbedo;
                pines.Density = 0.02f;
                pines.SplatmapChannel = -1;
                pines.MinSlopeAngle = 0.0f;
                pines.MaxSlopeAngle = 60.0f;
                pines.MinScale = 1.0f;
                pines.MaxScale = 1.0f;
                // Sizeable plants: the mesh and the card have to be
                // distinguishable at the near camera's distance.
                pines.MinHeight = 8.0f;
                pines.MaxHeight = 12.0f;
                pines.ViewDistance = 400.0f;
                pines.FadeStartDistance = 360.0f;
                pines.UseAuthoredMesh = true;
                pines.MeshViewDistance = kMeshViewDistance;
                pines.MeshFadeStartDistance = kMeshFadeStart;
                pines.AlphaCutoff = 0.25f;
                pines.WindStrength = 2.0f;
                pines.WindStiffness = 0.4f;
                pines.WindBranchWeight = 0.7f;
                pines.WindLeafWeight = 0.8f;
                pines.BaseColor = glm::vec3(0.18f, 0.42f, 0.14f);
                foliage.m_Layers.push_back(pines);
                foliage.m_NeedsRebuild = true;
            }
        }

        // Renders from `eye` and reads SceneColor, where the foliage pass
        // composites before post/UI.
        void Capture(const glm::vec3& eye, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);
            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px)
        {
            ASSERT_EQ(px.size(), static_cast<sizet>(kWidth) * kHeight * 4);
            std::vector<u8> flipped(px);
            VisualEvidence::FlipRgbaRowsInPlace(flipped, kWidth, kHeight);
            fs::path dir = fs::path("assets") / "tests" / "visual";
            if (!Options().GoldenVendor.empty())
                dir /= Options().GoldenVendor;
            if (!GoldenRebaseRequested())
            {
                ::stbi_set_flip_vertically_on_load(0);
                ::stbi_set_flip_vertically_on_load_thread(0);
                int width = 0, height = 0, channels = 0;
                auto* data = ::stbi_load((dir / name).string().c_str(), &width, &height, &channels, 4);
                ASSERT_TRUE(data) << "Missing golden " << name;
                std::vector<u8> baseline;
                if (width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight))
                    baseline.assign(data, data + static_cast<sizet>(kWidth) * kHeight * 4);
                ::stbi_image_free(data);
                ASSERT_EQ(baseline.size(), flipped.size()) << name;
                const auto rmse = VisualEvidence::Rgba8Rmse(flipped, baseline) / 255.0;
                if (rmse < 0.002)
                    return;
                EXPECT_LE(rmse, 0.05) << name;
                EXPECT_GE(VisualEvidence::Rgba8Ssim(flipped, baseline, kWidth, kHeight), 0.985f) << name;
                return;
            }
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << ec.message();
            ASSERT_NE(::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       flipped.data(), static_cast<int>(kWidth) * 4),
                      0);
        }

        void SetWind(bool on)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            foliage.m_Layers[0].WindStrength = on ? 2.0f : 0.0f;
            foliage.m_NeedsRebuild = true;
        }

        std::vector<f32> Velocity()
        {
            std::vector<f32> values;
            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::Velocity);
            EXPECT_NE(texture, 0u);
            if (texture != 0)
                ReadbackRgbaFloat(texture, kWidth, kHeight, values);
            return values;
        }

        std::vector<f32> Shadow()
        {
            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::ShadowMapCSMCascade0);
            EXPECT_NE(texture, 0u);
            if (texture == 0)
                return {};
            GLint width = 0, height = 0;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
            std::vector<f32> values(static_cast<sizet>(width) * static_cast<sizet>(height));
            glGetTextureSubImage(texture, 0, 0, 0, 0, width, height, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                                 static_cast<GLsizei>(values.size() * sizeof(f32)), values.data());
            return values;
        }

        static void WriteVelocity(const std::string& name, const std::vector<f32>& values)
        {
            std::vector<u8> pixels(values.size());
            for (sizet i = 0; i + 3 < values.size(); i += 4)
            {
                pixels[i] = static_cast<u8>(std::clamp(0.5f + 50.0f * values[i], 0.0f, 1.0f) * 255.0f);
                pixels[i + 1] = static_cast<u8>(std::clamp(0.5f + 50.0f * values[i + 1], 0.0f, 1.0f) * 255.0f);
                pixels[i + 2] = 0;
                pixels[i + 3] = 255;
            }
            WritePng(name, pixels);
        }

        Entity m_TerrainEntity;
    };

    TEST_F(FoliageWindEvidenceTest, EveryRasterPathShowsWindAndMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct MockClock
        {
            ~MockClock()
            {
                Time::ClearMockTime();
            }
        } clock;
        f32 time = 4.0f;
        const glm::vec3 eye(128.0f, 12.0f, 150.0f);
        for (const auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            const std::string pathName = path == RenderingPath::Forward ? "Forward" : path == RenderingPath::ForwardPlus ? "ForwardPlus"
                                                                                                                         : "Deferred";
            for (const u32 samples : { 1u, 4u })
            {
                // Only the deferred G-Buffer currently supports MSAA.
                if (samples > 1 && path != RenderingPath::Deferred)
                    continue;
                SCOPED_TRACE(pathName + " samples=" + std::to_string(samples));
                auto& settings = Renderer3D::GetRendererSettings();
                settings.Path = path;
                settings.Deferred.MSAASampleCount = samples;
                Renderer3D::ApplyRendererSettings();
                const std::string cell = "GL_" + pathName + "_MSAA" + std::to_string(samples);
                Time::SetMockTime(time);
                std::vector<u8> groundCapture;
                for (const bool oblique : { false, true })
                {
                    const glm::vec3 pose = oblique ? glm::vec3(160.0f, 22.0f, 160.0f) : eye;
                    const f32 yaw = oblique ? 0.5f : 0.0f;
                    const f32 pitch = oblique ? 0.3f : 0.06f;
                    std::vector<u8> on, repeat, off;
                    SetWind(true);
                    Capture(pose, yaw, pitch, on);
                    Capture(pose, yaw, pitch, repeat);
                    SetWind(false);
                    Capture(pose, yaw, pitch, off);
                    const f64 noise = VisualEvidence::Rgba8Rmse(on, repeat);
                    VisualEvidence::ExpectCapturesAreDistinct({ on, off }, { "wind", "off" }, noise);
                    const std::string angle = oblique ? "Oblique" : "Ground";
                    const auto isPlant = [](u32 r, u32 g, u32 b)
                    { return g > 12u && g > 1.4 * r && g > 1.4 * b; };
                    VisualEvidence::ExpectFrameHasSubject(on, angle, isPlant);
                    VisualEvidence::ExpectFrameHasSubject(off, angle + " off", isPlant);
                    if (oblique)
                        VisualEvidence::ExpectCapturesAreDistinct({ groundCapture, on }, { "ground", "oblique" }, noise);
                    else
                        groundCapture = on;
                    WritePng("FoliageWind_" + cell + "_" + angle + ".png", on);
                    WritePng("FoliageWindOff_" + cell + "_" + angle + ".png", off);
                }
                SetWind(true);
                std::vector<u8> warm;
                Capture(eye, 0.0f, 0.06f, warm);
                EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.5f, 2000.0f);
                camera.SetViewportSize(kWidth, kHeight);
                camera.SetPose(eye, 0.0f, 0.06f);
                time += 1.0f / 30.0f;
                Time::SetMockTime(time);
                RunEditorFrames(camera, 1);
                const auto moving = Velocity();
                GetScene().SetPaused(true);
                time += 1.0f / 30.0f;
                Time::SetMockTime(time);
                RunEditorFrames(camera, 1);
                const auto paused = Velocity();
                GetScene().SetPaused(false);
                ASSERT_EQ(moving.size(), paused.size());
                ASSERT_FALSE(moving.empty());
                sizet changed = 0;
                for (sizet i = 0; i + 3 < moving.size(); i += 4)
                {
                    EXPECT_TRUE(std::isfinite(moving[i]) && std::isfinite(moving[i + 1]));
                    EXPECT_TRUE(std::isfinite(paused[i]) && std::isfinite(paused[i + 1]));
                    EXPECT_LT(std::abs(paused[i]), 1e-4f);
                    EXPECT_LT(std::abs(paused[i + 1]), 1e-4f);
                    if (std::abs(moving[i] - paused[i]) + std::abs(moving[i + 1] - paused[i + 1]) > 1e-5f)
                        ++changed;
                }
                EXPECT_GT(changed, 20u) << "wind motion did not reach the velocity attachment";
                if (GoldenRebaseRequested())
                {
                    WriteVelocity("FoliageWind_" + cell + "_Velocity.png", moving);
                    WriteVelocity("FoliageWind_" + cell + "_Pause.png", paused);
                }
            }
        }
    }

    TEST_F(FoliageWindEvidenceTest, ForwardExportsRespectAmbientOcclusionDepthReaders)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus })
            for (const auto ao : { AOTechnique::SSAO, AOTechnique::GTAO })
            {
                auto& settings = Renderer3D::GetRendererSettings();
                settings.Path = path;
                auto& post = Renderer3D::GetPostProcessSettings();
                post.ActiveAOTechnique = ao;
                post.SSAOEnabled = ao == AOTechnique::SSAO;
                post.GTAOEnabled = ao == AOTechnique::GTAO;
                Renderer3D::ApplyRendererSettings();
                std::vector<u8> pixels;
                Capture({ 128.0f, 12.0f, 150.0f }, 0.0f, 0.06f, pixels);
                VisualEvidence::ExpectFrameHasSubject(pixels, "forward AO", [](u32 r, u32 g, u32 b)
                                                      { return g > 12u && g > 1.4 * r && g > 1.4 * b; });
            }
    }

    TEST_F(FoliageWindEvidenceTest, MissingAlbedoShadowDoesNotReuseThePreviousAtlas)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        auto& layer = foliage.m_Layers[0];
        layer.AlbedoPath.clear();
        layer.UseAuthoredMesh = false;
        layer.UseImpostor = false;
        layer.WindStrength = 0.0f;
        foliage.m_NeedsRebuild = true;
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.5f, 2000.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose({ 128.0f, 14.0f, 196.0f }, 0.0f, 6.0f);
        RunEditorFrames(camera, 4);
        ASSERT_TRUE(foliage.m_Renderer);
        const auto draws = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_FALSE(draws.empty());
        ASSERT_TRUE(std::ranges::all_of(draws, [](const auto& draw)
                                        { return draw.InstanceCount > 0 && !draw.AlbedoTextureID.IsValid(); }));

        GLStateGuard guard("MissingAlbedoShadow", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = spec.Height = 256;
        spec.Attachments = { FramebufferTextureFormat::DEPTH_COMPONENT32F };
        auto target = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/Foliage_Depth.glsl");
        ASSERT_TRUE(target && shader && shader->IsReady());
        auto cameraBuffer = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        UBOStructures::CameraUBO light{};
        const glm::vec3 origin = Renderer3D::GetRenderOrigin();
        light.View = glm::lookAt(glm::vec3(128.0f, 80.0f, 300.0f) - origin,
                                 glm::vec3(128.0f, 8.0f, 128.0f) - origin, glm::vec3(0.0f, 1.0f, 0.0f));
        light.Projection = glm::ortho(-150.0f, 150.0f, -150.0f, 150.0f, 1.0f, 600.0f);
        light.ViewProjection = light.Projection * light.View;
        cameraBuffer->SetData(&light, UBOStructures::CameraUBO::GetSize());
        TextureSpecification textureSpec;
        textureSpec.GenerateMips = false;
        auto transparent = Texture2D::Create(textureSpec);
        std::array<u8, 4> transparentPixel{ 255, 255, 255, 0 };
        transparent->SetData(transparentPixel.data(), static_cast<u32>(transparentPixel.size()));
        const auto capture = [&](const Ref<Texture2D>& previouslyBound)
        {
            target->Bind();
            glViewport(0, 0, 256, 256);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glClearDepth(1.0);
            glClear(GL_DEPTH_BUFFER_BIT);
            cameraBuffer->Bind();
            HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE,
                                             previouslyBound->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
            foliage.m_Renderer->RenderShadows(shader, 0.0f);
            std::vector<f32> depths(256 * 256);
            glGetTextureImage(target->GetDepthAttachmentRendererID(), 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                              static_cast<GLsizei>(depths.size() * sizeof(f32)), depths.data());
            target->Unbind();
            return depths;
        };
        const auto afterTransparentAtlas = capture(transparent);
        const auto afterOpaqueTexture = capture(Renderer3D::GetWhiteTexture());
        sizet covered = 0;
        f32 maximumDifference = 0.0f;
        for (sizet i = 0; i < afterOpaqueTexture.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(afterOpaqueTexture[i]) && std::isfinite(afterTransparentAtlas[i]));
            covered += afterOpaqueTexture[i] < 0.99999f;
            maximumDifference = std::max(maximumDifference, std::abs(afterOpaqueTexture[i] - afterTransparentAtlas[i]));
        }
        EXPECT_GT(covered, 20u) << "positive control rendered no foliage shadow";
        EXPECT_LE(maximumDifference, 1e-6f) << "null-albedo shadow depends on the previous atlas alpha";
    }

    TEST_F(FoliageWindEvidenceTest, ShadowImpostorAndLodResetFollowTheDeformation)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct MockClock
        {
            ~MockClock()
            {
                Time::ClearMockTime();
            }
        } clock;
        f32 time = 4.0f;
        Time::SetMockTime(time);
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        settings.Deferred.MSAASampleCount = 1;
        Renderer3D::ApplyRendererSettings();
        const glm::vec3 eye(128.0f, 12.0f, 150.0f);
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.5f, 2000.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose(eye, 0.0f, 0.06f);
        RunEditorFrames(camera, 4);
        const auto before = Shadow();
        ASSERT_FALSE(before.empty());
        // Several deterministic increments move the real shadow silhouette.
        for (u32 frame = 0; frame < 12; ++frame)
        {
            time += 1.0f / 30.0f;
            Time::SetMockTime(time);
            RunEditorFrames(camera, 1);
        }
        const auto after = Shadow();
        ASSERT_EQ(before.size(), after.size());
        sizet changed = 0;
        for (sizet i = 0; i < before.size(); ++i)
            if (std::abs(before[i] - after[i]) > 1e-6f)
                ++changed;
        EXPECT_GT(changed, 20u) << "wind changed colour but not the real CSM depth";

        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        foliage.m_Layers[0].UseImpostor = true;
        foliage.m_Layers[0].ImpostorFramesPerAxis = 4;
        foliage.m_Layers[0].ImpostorAtlasResolution = 256;
        foliage.m_NeedsRebuild = true;
        std::vector<u8> distantCapture;
        Capture({ 128.0f, 50.0f, 360.0f }, 0.0f, 0.22f, distantCapture);
        if (GoldenRebaseRequested())
            WritePng("FoliageWind_GL_Deferred_Impostor.png", distantCapture);
        auto info = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_TRUE(std::ranges::any_of(info, [](const auto& draw)
                                        { return draw.UseImpostor; }));

        // LOD authoring changes reset wind once. Reprojecting the same new
        // representation must not invent motion from the discarded geometry.
        RunEditorFrames(camera, 4); // settle the near camera before testing representation history
        foliage.m_Layers[0].UseAuthoredMesh = false;
        foliage.m_NeedsRebuild = true;
        time += 1.0f / 30.0f;
        Time::SetMockTime(time);
        RunEditorFrames(camera, 1);
        const auto reset = Velocity();
        ASSERT_FALSE(reset.empty());
        f32 maximum = 0.0f;
        for (sizet i = 0; i + 3 < reset.size(); i += 4)
            maximum = std::max(maximum, std::abs(reset[i]) + std::abs(reset[i + 1]));
        EXPECT_LT(maximum, 1e-4f);
        if (GoldenRebaseRequested())
            WriteVelocity("FoliageWind_GL_Deferred_LodReset.png", reset);
    }
} // namespace OloEngine::Tests
