// OLO_TEST_LAYER: L8
// #1524: the real Scene submission and shadow pipeline, with each family
// isolated above a mesh receiver. Shadow position is measured on the floor,
// outside the caster's footprint; turning casting off preserves its geometry.
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include <glad/gl.h>
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "TestTempDir.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include <stb_image/stb_image_write.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <array>
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;
        enum class Family
        {
            Terrain,
            Voxel,
            GreedyVoxel,
            FoliageCard,
            FoliageMesh
        };
        const char* FamilyName(Family family)
        {
            switch (family)
            {
                case Family::Terrain:
                    return "Terrain";
                case Family::Voxel:
                    return "Voxel";
                case Family::GreedyVoxel:
                    return "GreedyVoxel";
                case Family::FoliageCard:
                    return "FoliageCard";
                case Family::FoliageMesh:
                    return "FoliageMesh";
            }
            return "Unknown";
        }
        const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Unknown";
            }
        }
        constexpr std::array<const char*, 3> kLightNames{ "Sun", "Spot", "Point" };
        constexpr std::array<const char*, 3> kModeNames{ "CSMAtlas", "VSMSunAtlas", "VSMSunLocal" };
        struct PresentationVariant
        {
            const char* Name;
            u32 Samples;
            UpscaleMode Upscale;
            UpscalerTechnique Technique;
            u32 Width;
            u32 Height;
        };
        constexpr std::array<PresentationVariant, 11> kPresentationVariants{
            PresentationVariant{ "MSAA4", 4, UpscaleMode::Off, UpscalerTechnique::Spatial, kWidth, kHeight },
            { "FSR1Quality", 1, UpscaleMode::Quality, UpscalerTechnique::Spatial, kWidth, kHeight },
            { "FSR2Quality", 1, UpscaleMode::Quality, UpscalerTechnique::Temporal, kWidth, kHeight },
            { "853x479Native", 1, UpscaleMode::Off, UpscalerTechnique::Spatial, 853, 479 },
            { "MSAA4FSR1Quality", 4, UpscaleMode::Quality, UpscalerTechnique::Spatial, kWidth, kHeight },
            { "MSAA4FSR2Quality", 4, UpscaleMode::Quality, UpscalerTechnique::Temporal, kWidth, kHeight },
            { "853x479MSAA4Native", 4, UpscaleMode::Off, UpscalerTechnique::Spatial, 853, 479 },
            { "853x479FSR1Quality", 1, UpscaleMode::Quality, UpscalerTechnique::Spatial, 853, 479 },
            { "853x479FSR2Quality", 1, UpscaleMode::Quality, UpscalerTechnique::Temporal, 853, 479 },
            { "853x479MSAA4FSR1Quality", 4, UpscaleMode::Quality, UpscalerTechnique::Spatial, 853, 479 },
            { "853x479MSAA4FSR2Quality", 4, UpscaleMode::Quality, UpscalerTechnique::Temporal, 853, 479 }
        };
        struct EvidenceFrame
        {
            std::vector<u8> Pixels; // ReadbackComposite API boundary
            TArray<i32> EntityIDs;
            u32 IDWidth = 0;
            u32 IDHeight = 0;
            u32 ActiveIDWidth = 0;
            u32 ActiveIDHeight = 0;
        };
        struct ShadowMoment
        {
            u32 Pixels = 0;
            f64 Mass = 0.0;
            glm::dvec2 Position{ 0.0 };
        };
    } // namespace

    class ShadowFamilyVisualEvidence : public RendererAttachedTest
    {
      protected:
        u32 m_Width = kWidth;
        u32 m_Height = kHeight;
        f32 m_ReceiverMinX = 3.0f;
        i32 m_FloorID = -1;
        Entity m_Caster;
        Entity m_Light;
        bool m_ImpostorEvidence = false;
        bool m_MeasurementsStarted = false;
        bool m_UnsupportedStarted = false;

        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Renderer3D::GetPostProcessSettings() = PostProcessSettings{};
            auto& settings = Renderer3D::GetRendererSettings();
            settings.ForwardPlusAutoSwitch = false;
            settings.EditorDebugDrawsEnabled = false;
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowCameraFrustums = false;
            Entity floor = GetScene().CreateEntity("Mesh receiver");
            m_FloorID = static_cast<i32>(static_cast<u32>(floor));
            floor.GetComponent<TransformComponent>().Scale = { 60.0f, 1.0f, 60.0f };
            auto& mesh = floor.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Plane;
            mesh.m_MeshSource = MeshPrimitives::CreatePlane()->GetMeshSource();
            auto& material = floor.AddComponent<MaterialComponent>().m_Material;
            material.SetBaseColorFactor(glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
            material.SetRoughnessFactor(1.0f);
            material.SetMetallicFactor(0.0f);
            auto runtimeCamera = GetScene().CreateEntity("Runtime evidence camera");
            auto& cameraTransform = runtimeCamera.GetComponent<TransformComponent>();
            cameraTransform.Translation = { 4, 14, 26 };
            cameraTransform.SetRotationEuler({ -0.45f, 0, 0 });
            auto& camera = runtimeCamera.AddComponent<CameraComponent>();
            camera.Camera.SetPerspective(glm::radians(45.0f), 0.1f, 200.0f);
            camera.Primary = true;
        }

        void AddFamily(Family family)
        {
            m_ReceiverMinX = family == Family::FoliageMesh ? 6.0f : 3.0f;
            m_Caster = GetScene().CreateEntity(FamilyName(family));
            auto& terrain = m_Caster.AddComponent<TerrainComponent>();
            terrain.m_WorldSizeX = 4.0f;
            terrain.m_WorldSizeZ = 4.0f;
            terrain.m_HeightScale = 8.0f;
            terrain.m_CollisionEnabled = false;
            terrain.m_TerrainData = Ref<TerrainData>::Create();
            terrain.m_TerrainData->CreateFlat(65, 0.0f);
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            auto& transform = m_Caster.GetComponent<TransformComponent>();
            transform.Translation = { -2.0f, -1.0f, -2.0f };
            if (family == Family::Terrain)
            {
                // A single-sided displaced heightfield, suspended above the
                // receiver so its own visible surface cannot hide its shadow.
                transform.Translation.y = 4.0f;
                terrain.m_TessellationEnabled = true;
                auto& heights = terrain.m_TerrainData->GetHeightData();
                for (u32 z = 0; z < 65; ++z)
                    for (u32 x = 0; x < 65; ++x)
                        heights[z * 65 + x] = 0.2f + 0.15f * std::sin(x * 0.19f) * std::sin(z * 0.23f);
                terrain.m_TerrainData->UploadToGPU();
            }
            else if (family == Family::Voxel || family == Family::GreedyVoxel)
            {
                terrain.m_VoxelEnabled = true;
                terrain.m_VoxelSize = 0.25f;
                terrain.m_VoxelOverride = Ref<VoxelOverride>::Create();
                terrain.m_VoxelOverride->Initialize(4.0f, 4.0f, 8.0f, 0.25f);
                terrain.m_VoxelOverride->AddSphere({ 2.0f, 5.0f, 2.0f }, 1.8f);
                terrain.m_VoxelMesher = family == Family::GreedyVoxel ? VoxelMesherKind::GreedyCubic : VoxelMesherKind::MarchingCubes;
            }
            else
            {
                auto& foliage = m_Caster.AddComponent<FoliageComponent>();
                foliage.m_Layers.SetNum(1);
                auto& layer = foliage.m_Layers[0];
                layer.Density = 0.5f;
                layer.SplatmapChannel = -1;
                layer.MinHeight = layer.MaxHeight = 6.0f;
                layer.MinScale = layer.MaxScale = 1.0f;
                layer.WindStrength = 0.0f;
                layer.InteractionResponse = 0.0f;
                layer.ViewDistance = 100.0f;
                layer.FadeStartDistance = 90.0f;
                layer.CastShadows = true;
                if (family == Family::FoliageMesh)
                {
                    layer.UseAuthoredMesh = true;
                    layer.MeshPath = "SandboxProject/Assets/Models/Vegetation/shrub/shrub.obj";
                    layer.MeshViewDistance = 100.0f;
                    layer.MeshFadeStartDistance = 90.0f;
                    if (m_ImpostorEvidence)
                    {
                        layer.UseImpostor = true;
                        layer.MeshViewDistance = layer.MeshFadeStartDistance = 0.0f;
                        layer.ImpostorStartDistance = 0.0f;
                        layer.ImpostorTransitionBand = 0.01f;
                        layer.ImpostorFramesPerAxis = 4;
                        layer.ImpostorAtlasResolution = 256;
                    }
                }
            }
        }

        void SetLight(u32 kind)
        {
            if (m_Light)
                GetScene().DestroyEntity(m_Light);
            m_Light = GetScene().CreateEntity("Shadow light");
            m_Light.GetComponent<TransformComponent>().Translation = { -6.0f, 10.0f, 0.0f };
            if (kind == 0)
            {
                auto& light = m_Light.AddComponent<DirectionalLightComponent>();
                light.m_Direction = glm::normalize(glm::vec3(0.7f, -0.45f, 0.0f));
                light.m_Intensity = 2.0f;
                light.m_CastShadows = true;
            }
            else if (kind == 1)
            {
                auto& light = m_Light.AddComponent<SpotLightComponent>();
                light.m_Direction = glm::normalize(glm::vec3(6.0f, -8.0f, 0.0f));
                light.m_InnerCutoff = 55.0f;
                light.m_OuterCutoff = 70.0f;
                light.m_Range = 45.0f;
                // Keep the isolated lamp's inverse-square irradiance measurable
                // on the distant receiver without lowering the pixel oracle.
                light.m_Intensity = 500.0f;
                light.m_CastShadows = true;
            }
            else
            {
                auto& light = m_Light.AddComponent<PointLightComponent>();
                light.m_Range = 45.0f;
                // Keep the isolated lamp's inverse-square irradiance measurable
                // on the distant receiver without lowering the pixel oracle.
                light.m_Intensity = 500.0f;
                light.m_CastShadows = true;
            }
        }

        void ExportLiveFixture(Family family, u32 light, const char* prefix = "ShadowFamilyFixture")
        {
            const auto directory = std::filesystem::path(OLO_TEST_EDITOR_ROOT) / "assets/tests/visual";
            std::filesystem::create_directories(directory);
            auto& terrain = m_Caster.GetComponent<TerrainComponent>();
            const auto heightPath = directory / (std::string(prefix) + "_" + FamilyName(family) + "_Height.png");
            TArray<u8> heights;
            for (const auto height : std::as_const(*terrain.m_TerrainData).GetHeightData())
                heights.Add(static_cast<u8>(std::clamp(height, 0.0f, 1.0f) * 255.0f));
            stbi_flip_vertically_on_write(0);
            EXPECT_NE(stbi_write_png(heightPath.string().c_str(), 65, 65, 1, heights.GetData(), 65), 0);
            terrain.m_HeightmapPath = std::filesystem::relative(heightPath, std::filesystem::path(OLO_TEST_EDITOR_ROOT)).generic_string();
            if (terrain.m_VoxelOverride)
            {
                const auto bytes = terrain.m_VoxelOverride->SerializeRLE();
                std::ofstream volume(directory / (std::string(prefix) + "_" + FamilyName(family) + ".vox1"), std::ios::binary);
                volume.write(reinterpret_cast<const char*>(bytes.GetData()), static_cast<std::streamsize>(bytes.Num()));
                EXPECT_TRUE(volume.good());
            }
            const std::string name = std::string(prefix) + "_" + FamilyName(family) + "_" + kLightNames[light];
            GetScene().SetName(name);
            SceneSerializer(GetSceneRef()).Serialize(directory / (name + ".olo"));
        }

        void Cast(bool casts)
        {
            if (m_Light.HasComponent<DirectionalLightComponent>())
                m_Light.GetComponent<DirectionalLightComponent>().m_CastShadows = casts;
            else if (m_Light.HasComponent<SpotLightComponent>())
                m_Light.GetComponent<SpotLightComponent>().m_CastShadows = casts;
            else
                m_Light.GetComponent<PointLightComponent>().m_CastShadows = casts;
        }

        EvidenceFrame Capture(const EditorCamera& camera, const std::string& name)
        {
            const auto& post = Renderer3D::GetPostProcessSettings();
            // Temporal history needs to converge after the casting A/B toggle;
            // eight frames erased the thin impostor shadow in both techniques.
            const u32 settleFrames = post.Technique == UpscalerTechnique::Temporal && post.Upscale != UpscaleMode::Off ? 64u : 8u;
            RunEditorFrames(camera, settleFrames, 0.0f); // pinned clock, including wind
            EvidenceFrame frame;
            u32 width = 0, height = 0;
            EXPECT_TRUE(ReadbackComposite(frame.Pixels, width, height));
            EXPECT_EQ(width, m_Width);
            EXPECT_EQ(height, m_Height);
            if (!frame.Pixels.empty())
            {
                const auto directory = std::filesystem::path(OLO_TEST_EDITOR_ROOT) / "assets/tests/visual";
                std::filesystem::create_directories(directory);
                stbi_flip_vertically_on_write(1);
                EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(), width, height, 4,
                                         frame.Pixels.data(), width * 4),
                          0);
            }
            // Restrict the oracle to the actual receiver, including deformation
            // controls where a leaf can move across the ray's floor intersection.
            const u32 ids = Renderer3D::ResolveFrameGraphTexture(ResourceNames::SceneEntityID);
            EXPECT_NE(ids, 0u);
            if (ids != 0u)
            {
                GLint idWidth = 0, idHeight = 0;
                glGetTextureLevelParameteriv(ids, 0, GL_TEXTURE_WIDTH, &idWidth);
                glGetTextureLevelParameteriv(ids, 0, GL_TEXTURE_HEIGHT, &idHeight);
                if (idWidth > 0 && idHeight > 0)
                {
                    frame.IDWidth = static_cast<u32>(idWidth);
                    frame.IDHeight = static_cast<u32>(idHeight);
                    const auto receiver = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
                    EXPECT_TRUE(receiver);
                    frame.ActiveIDWidth = receiver ? std::min(frame.IDWidth, receiver->GetSpecification().Width) : frame.IDWidth;
                    frame.ActiveIDHeight = receiver ? std::min(frame.IDHeight, receiver->GetSpecification().Height) : frame.IDHeight;
                    frame.EntityIDs.SetNum(static_cast<sizet>(frame.IDWidth) * frame.IDHeight);
                    glGetTextureImage(ids, 0, GL_RED_INTEGER, GL_INT,
                                      static_cast<GLsizei>(frame.EntityIDs.Num() * sizeof(i32)), frame.EntityIDs.GetData());
                    EXPECT_EQ(glGetError(), GL_NO_ERROR);
                }
            }
            return frame;
        }

        ShadowMoment Measure(const EditorCamera& camera, const EvidenceFrame& lit, const EvidenceFrame& shadowed)
        {
            ShadowMoment moment;
            const glm::mat4 inverse = glm::inverse(camera.GetViewProjection());
            if (lit.Pixels.size() != m_Width * m_Height * 4u || shadowed.Pixels.size() != lit.Pixels.size() || lit.EntityIDs.IsEmpty() || shadowed.EntityIDs.IsEmpty())
                return moment;
            for (u32 y = 0; y < m_Height; ++y)
            {
                for (u32 x = 0; x < m_Width; ++x)
                {
                    const glm::vec2 ndc = glm::vec2((x + 0.5f) / m_Width, (y + 0.5f) / m_Height) * 2.0f - 1.0f;
                    glm::vec4 nearPoint = inverse * glm::vec4(ndc, -1, 1);
                    glm::vec4 farPoint = inverse * glm::vec4(ndc, 1, 1);
                    nearPoint /= nearPoint.w;
                    farPoint /= farPoint.w;
                    const glm::vec3 ray = glm::vec3(farPoint - nearPoint);
                    if (std::abs(ray.y) < 1e-6f)
                        continue;
                    const f32 t = -nearPoint.y / ray.y;
                    const glm::vec3 floor = glm::vec3(nearPoint) + ray * t;
                    if (t <= 0 || floor.x < m_ReceiverMinX || floor.x > 16.0f || std::abs(floor.z) > 8.0f)
                        continue;
                    const auto onReceiver = [&](const EvidenceFrame& frame)
                    {
                        const u32 ix = std::min(frame.ActiveIDWidth - 1, static_cast<u32>((x + 0.5f) * frame.ActiveIDWidth / m_Width));
                        const u32 iy = std::min(frame.ActiveIDHeight - 1, static_cast<u32>((y + 0.5f) * frame.ActiveIDHeight / m_Height));
                        return frame.EntityIDs[static_cast<sizet>(iy) * frame.IDWidth + ix] == m_FloorID;
                    };
                    if (!onReceiver(lit) || !onReceiver(shadowed))
                        continue;
                    const sizet at = (static_cast<sizet>(y) * m_Width + x) * 4u;
                    const auto luma = [at](const EvidenceFrame& frame)
                    { return 0.2126 * frame.Pixels[at] + 0.7152 * frame.Pixels[at + 1] + 0.0722 * frame.Pixels[at + 2]; };
                    const f64 delta = luma(lit) - luma(shadowed);
                    if (delta < 8.0)
                        continue;
                    ++moment.Pixels;
                    moment.Mass += delta;
                    moment.Position += glm::dvec2(floor.x, floor.z) * delta;
                }
            }
            if (moment.Mass > 0.0)
                moment.Position /= moment.Mass;
            return moment;
        }

        void DeformationAndCameraLifecycle(Family family, const EditorCamera& camera,
                                           const EvidenceFrame& lit, const EvidenceFrame& original,
                                           const ShadowMoment& baseline, const std::string& cell)
        {
            auto& terrain = m_Caster.GetComponent<TerrainComponent>();
            TArray<u8> savedVoxels;
            if (family == Family::Terrain)
            {
                for (auto& height : terrain.m_TerrainData->GetHeightData())
                    height += 0.2f;
                terrain.m_TerrainData->UploadToGPU();
            }
            else if (family == Family::Voxel || family == Family::GreedyVoxel)
            {
                savedVoxels = terrain.m_VoxelOverride->SerializeRLE();
                terrain.m_VoxelOverride->AddSphere({ 2, 7, 2 }, 1.0f);
            }
            else
            {
                auto& foliage = m_Caster.GetComponent<FoliageComponent>();
                foliage.m_Layers[0].WindStrength = 2.0f;
                foliage.m_NeedsRebuild = true;
                RunEditorFrames(camera, 12, 0.25f);
            }
            const auto deformed = Capture(camera, "ShadowFamilyDeformed_" + cell);
            const auto darker = Measure(camera, original, deformed);
            const auto lighter = Measure(camera, deformed, original);
            EXPECT_GT(darker.Mass + lighter.Mass, 200.0) << "in-place deformation retained the old shadow";
            if (family == Family::Terrain)
            {
                for (auto& height : terrain.m_TerrainData->GetHeightData())
                    height -= 0.2f;
                terrain.m_TerrainData->UploadToGPU();
            }
            else if (family == Family::Voxel || family == Family::GreedyVoxel)
                ASSERT_TRUE(terrain.m_VoxelOverride->DeserializeRLE(savedVoxels));
            else
            {
                auto& foliage = m_Caster.GetComponent<FoliageComponent>();
                foliage.m_Layers[0].WindStrength = 0.0f;
                foliage.m_NeedsRebuild = true;
            }
            const auto restored = Capture(camera, "ShadowFamilyDeformationRestored_" + cell);
            EXPECT_GT(Measure(camera, lit, restored).Mass, baseline.Mass * 0.7);
            EditorCamera cut(45.0f, static_cast<f32>(m_Width) / m_Height, 0.1f, 200.0f);
            cut.SetViewportSize(m_Width, m_Height);
            cut.SetPose({ 400, 30, 300 }, 0.0f, 0.45f);
            RunEditorFrames(cut, 4, 0.0f); // cross many clip-map page ownership boundaries
            const auto afterCut = Capture(camera, "ShadowFamilyCameraReturned_" + cell);
            const auto moment = Measure(camera, lit, afterCut);
            EXPECT_GT(moment.Mass, baseline.Mass * 0.7);
            if (moment.Pixels > 30 && baseline.Pixels > 30)
                EXPECT_LT(glm::length(moment.Position - baseline.Position), 0.5);
        }

        void RecordUnsupported(Family family, const std::string& reason)
        {
            const auto directory = std::filesystem::path(OLO_TEST_EDITOR_ROOT) / "assets/tests/visual";
            std::filesystem::create_directories(directory);
            const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
            std::ofstream output(directory / (std::string("ShadowFamilyUnsupported_") + test->name() + ".txt"),
                                 m_UnsupportedStarted ? std::ios::app : std::ios::trunc);
            m_UnsupportedStarted = true;
            output << FamilyName(family) << ": " << reason << '\n';
        }

        void Record(const std::string& cell, const ShadowMoment& moment)
        {
            const auto directory = std::filesystem::path(OLO_TEST_EDITOR_ROOT) / "assets/tests/visual";
            std::filesystem::create_directories(directory);
            const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
            std::ofstream output(directory / (std::string("ShadowFamilyMeasurements_") + test->name() + ".csv"),
                                 m_MeasurementsStarted ? std::ios::app : std::ios::trunc);
            if (!m_MeasurementsStarted)
                output << "cell,pixels,darknessMass,centroidX,centroidZ,width,height,sceneSamples,resolvedUpscaler,upscaleFallback,statsReady,pagesDrawn,pagesResident,localPagesDrawn,localPagesResident\n";
            m_MeasurementsStarted = true;
            const auto& vsm = Renderer3D::GetShadowMap().GetVirtualShadowMap();
            output << cell << ',' << moment.Pixels << ',' << moment.Mass << ','
                   << moment.Position.x << ',' << moment.Position.y << ',' << m_Width << ',' << m_Height << ','
                   << Renderer3D::GetUpscaleResolution().SceneSampleCount << ','
                   << static_cast<i32>(Renderer3D::GetUpscaleResolution().Result.Resolved) << ','
                   << static_cast<i32>(Renderer3D::GetUpscaleResolution().Result.Fallback) << ',' << vsm.HasStatistics() << ','
                   << vsm.GetStatistics().PagesDrawn << ',' << vsm.GetStatistics().PagesResident << ','
                   << vsm.GetStatistics().LocalPagesDrawn << ',' << vsm.GetStatistics().LocalPagesResident << '\n';
        }

        void PresentationEvidence(Family family)
        {
            AddFamily(family);
            // CI keeps the representative presentation grid; the retained local
            // evidence run expands every reachable path/light/angle/local cell.
            const bool fullMatrix = std::getenv("OLO_VSM_FAMILY_FULL_MATRIX") != nullptr;
            RecordUnsupported(family, "Bilinear upscale is not a selectable renderer technique; spatial mode is FSR1 EASU");
            auto& renderer = Renderer3D::GetRendererSettings();
            for (auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
            {
                if (!fullMatrix && path != RenderingPath::Deferred)
                    continue;
                renderer.Path = path;
                for (sizet variant = 0; variant < (fullMatrix ? kPresentationVariants.size() : 4u); ++variant)
                {
                    const auto& presentation = kPresentationVariants[variant];
                    if (presentation.Samples != 1 && path != RenderingPath::Deferred)
                    {
                        RecordUnsupported(family, "MSAA is a deferred G-buffer setting; Forward paths consume one scene sample");
                        continue;
                    }
                    renderer.Deferred.MSAASampleCount = presentation.Samples;
                    auto& post = Renderer3D::GetPostProcessSettings();
                    post.Upscale = presentation.Upscale;
                    post.Technique = presentation.Technique;
                    if (post.Technique == UpscalerTechnique::Temporal && !TemporalUpscalerUsable())
                    {
                        RecordUnsupported(family, "FSR2 unavailable on this GL device; Spatial is the supported fallback");
                        if (!fullMatrix)
                            continue;
                    }
                    if (presentation.Samples > Renderer3D::GetMaxMSAASamples())
                    {
                        RecordUnsupported(family, "4x MSAA exceeds driver cap " + std::to_string(Renderer3D::GetMaxMSAASamples()));
                        continue;
                    }
                    m_Width = presentation.Width;
                    m_Height = presentation.Height;
                    Renderer3D::ApplyRendererSettings();
                    ResizeRenderTarget(m_Width, m_Height);
                    for (u32 angle = 0; angle < (fullMatrix ? 2u : 1u); ++angle)
                    {
                        EditorCamera camera(45.0f, static_cast<f32>(m_Width) / m_Height, 0.1f, 200.0f);
                        camera.SetViewportSize(m_Width, m_Height);
                        camera.SetPose(angle == 0 ? glm::vec3(4, 14, 26) : glm::vec3(-14, 14, 20), angle == 0 ? 0.0f : 0.7f, 0.45f);
                        for (u32 light = 0; light < (fullMatrix ? 3u : 2u); ++light)
                        {
                            SetLight(light);
                            std::array<ShadowMoment, 3> moments;
                            const u32 modes = fullMatrix ? 3u : 2u;
                            for (u32 mode = 0; mode < modes; ++mode)
                            {
                                const std::string cell = std::string(FamilyName(family)) + (m_ImpostorEvidence ? "Impostor_GL_" : "_GL_") + PathName(path) +
                                                         "_" + presentation.Name + "_Angle" + std::to_string(angle) +
                                                         "_" + kLightNames[light] + "_" + kModeNames[fullMatrix ? mode : mode * 2];
                                SCOPED_TRACE(cell);
                                auto settings = Renderer3D::GetShadowMap().GetSettings();
                                settings.Enabled = true;
                                settings.VSM.Enabled = mode != 0;
                                settings.VSM.LocalLights = !fullMatrix || mode == 2;
                                Renderer3D::GetShadowMap().SetSettings(settings);
                                ASSERT_EQ(Renderer3D::GetShadowMap().IsVirtualShadowMapActive(), mode != 0);
                                Cast(false);
                                const auto lit = Capture(camera, "ShadowFamilyOff_" + cell);
                                Cast(true);
                                const auto shadowed = Capture(camera, "ShadowFamily_" + cell);
                                moments[mode] = Measure(camera, lit, shadowed);
                                Record(cell, moments[mode]);
                                EXPECT_GT(moments[mode].Pixels, 30u);
                                const auto& resolve = Renderer3D::GetUpscaleResolution();
                                ASSERT_TRUE(resolve.Latched);
                                EXPECT_EQ(resolve.Path, path);
                                EXPECT_EQ(resolve.SceneSampleCount, presentation.Samples);
                                if (post.Technique == UpscalerTechnique::Temporal && !Renderer3D::IsTemporalUpscaleActive())
                                    RecordUnsupported(family, cell + " FSR2 fell back: reason " + std::to_string(static_cast<i32>(resolve.Result.Fallback)));
                            }
                            for (u32 mode = 1; mode < modes; ++mode)
                            {
                                if (moments[0].Pixels > 30 && moments[mode].Pixels > 30)
                                {
                                    EXPECT_LT(glm::length(moments[0].Position - moments[mode].Position), 1.5);
                                    EXPECT_GT(moments[mode].Mass, moments[0].Mass * 0.15);
                                }
                            }
                        }
                    }
                }
            }
            GetScene().DestroyEntity(m_Caster);
            m_Width = kWidth;
            m_Height = kHeight;
            ResizeRenderTarget(m_Width, m_Height);
        }

        void Evidence(Family family)
        {
            AddFamily(family);
            for (u32 light = 0; light < 3; ++light)
            {
                SetLight(light);
                ExportLiveFixture(family, light, m_ImpostorEvidence ? "ShadowFamilyImpostorFixture" : "ShadowFamilyFixture");
                for (auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
                {
                    Renderer3D::GetRendererSettings().Path = path;
                    Renderer3D::ApplyRendererSettings();
                    for (u32 angle = 0; angle < 2; ++angle)
                    {
                        EditorCamera camera(45.0f, static_cast<f32>(kWidth) / kHeight, 0.1f, 200.0f);
                        camera.SetViewportSize(kWidth, kHeight);
                        camera.SetPose(angle == 0 ? glm::vec3(4, 14, 26) : glm::vec3(-14, 14, 20),
                                       angle == 0 ? 0.0f : 0.7f, 0.45f);
                        const std::string cell = std::string(FamilyName(family)) + (m_ImpostorEvidence ? "Impostor_GL_" : "_GL_") + PathName(path) +
                                                 "_" + kLightNames[light] + "_Angle" + std::to_string(angle);
                        SCOPED_TRACE(cell);
                        std::array<ShadowMoment, 3> moments;
                        for (u32 virtualMap = 0; virtualMap < 3; ++virtualMap)
                        {
                            auto settings = Renderer3D::GetShadowMap().GetSettings();
                            settings.Enabled = true;
                            settings.VSM.Enabled = virtualMap != 0;
                            settings.VSM.LocalLights = virtualMap == 2;
                            Renderer3D::GetShadowMap().SetSettings(settings);
                            ASSERT_EQ(Renderer3D::GetShadowMap().IsVirtualShadowMapActive(), virtualMap != 0);
                            Cast(false);
                            const auto lit = Capture(camera, "ShadowFamilyOff_" + cell + "_" + kModeNames[virtualMap]);
                            Cast(true);
                            const auto shadowed = Capture(camera, "ShadowFamily_" + cell + "_" + kModeNames[virtualMap]);
                            moments[virtualMap] = Measure(camera, lit, shadowed);
                            Record(cell + "_" + kModeNames[virtualMap], moments[virtualMap]);
                            // Reuse, motion and disappearance are measured without
                            // resetting the VSM between the live frame and control.
                            if (virtualMap == 2 && angle == 0)
                            {
                                if (light == 0 && path == RenderingPath::Deferred)
                                    DeformationAndCameraLifecycle(family, camera, lit, shadowed, moments[virtualMap], cell);

                                if (family == Family::Terrain || family == Family::Voxel || family == Family::GreedyVoxel)
                                {
                                    RunEditorFrames(camera, 12, 0.0f);
                                    auto& vsm = Renderer3D::GetShadowMap().GetVirtualShadowMap();
                                    ASSERT_TRUE(vsm.HasStatistics());
                                    EXPECT_EQ(light == 0 ? vsm.GetStatistics().PagesDrawn : vsm.GetStatistics().LocalPagesDrawn, 0u)
                                        << "static family continuously dirtied cached pages";
                                }
                                if (family == Family::FoliageCard || family == Family::FoliageMesh)
                                {
                                    auto& foliage = m_Caster.GetComponent<FoliageComponent>();
                                    foliage.m_Layers[0].CastShadows = false;
                                    foliage.m_NeedsRebuild = true;
                                    const auto disabled = Capture(camera, "ShadowFamilyCastFlagOff_" + cell);
                                    EXPECT_LT(Measure(camera, lit, disabled).Mass, moments[virtualMap].Mass * 0.1 + 50.0);
                                    foliage.m_Layers[0].CastShadows = true;
                                    foliage.m_NeedsRebuild = true;
                                    Capture(camera, "ShadowFamilyCastFlagOn_" + cell);
                                }
                                // Translate clear of the receiver's measured band;
                                // both old and new bounds must be dirtied.
                                m_Caster.GetComponent<TransformComponent>().Translation.z += 30.0f;
                                const auto moved = Capture(camera, "ShadowFamilyMoved_" + cell);
                                EXPECT_LT(Measure(camera, lit, moved).Mass, moments[virtualMap].Mass * 0.1 + 50.0);
                                m_Caster.GetComponent<TransformComponent>().Translation.z -= 30.0f;
                                const auto returned = Capture(camera, "ShadowFamilyReturned_" + cell);
                                EXPECT_GT(Measure(camera, lit, returned).Mass, moments[virtualMap].Mass * 0.7);
                                GetScene().DestroyEntity(m_Caster);
                                const auto removed = Capture(camera, "ShadowFamilyRemoved_" + cell);
                                EXPECT_LT(Measure(camera, lit, removed).Mass, moments[virtualMap].Mass * 0.1 + 50.0);
                                AddFamily(family);
                            }
                            EXPECT_GT(moments[virtualMap].Pixels, 30u) << "isolated family cast no measurable shadow";
                        }
                        for (u32 virtualMap = 1; virtualMap < 3; ++virtualMap)
                            if (moments[0].Pixels > 30 && moments[virtualMap].Pixels > 30)
                            {
                                EXPECT_LT(glm::length(moments[0].Position - moments[virtualMap].Position), 1.5)
                                    << "VSM shadow moved in world space relative to CSM/atlas";
                                EXPECT_GT(moments[virtualMap].Mass, moments[0].Mass * 0.15)
                                    << "VSM kept only a small fraction of this family's shadow";
                            }
                    }
                }
            }
            GetScene().DestroyEntity(m_Caster);
        }
    };

    TEST_F(ShadowFamilyVisualEvidence, StartupRequestsDoNotOverrideLaterWritesWhenResourcesResize)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct LeverGuard
        {
            Levers::Tristate Maps = Levers::VirtualShadowMapsStartup();
            Levers::Tristate Local = Levers::VirtualShadowLocalLightsStartup();
            ~LeverGuard()
            {
                Levers::SetVirtualShadowMapsStartup(Maps);
                Levers::SetVirtualShadowLocalLightsStartup(Local);
            }
        } leverGuard;
        Levers::SetVirtualShadowMapsStartup(Levers::Tristate::On);
        Levers::SetVirtualShadowLocalLightsStartup(Levers::Tristate::On);
        ShadowMap map;
        struct MapGuard
        {
            ShadowMap& Map;
            ~MapGuard()
            {
                Map.Shutdown();
            }
        } mapGuard{ map };
        ShadowSettings settings;
        settings.Resolution = 128;
        settings.AtlasResolution = 128;
        map.Init(settings);
        EXPECT_TRUE(map.GetSettings().VSM.Enabled);
        EXPECT_TRUE(map.GetSettings().VSM.LocalLights);
        settings = map.GetSettings();
        settings.VSM.Enabled = false;
        settings.VSM.LocalLights = false;
        map.SetSettings(settings);
        settings.Resolution = 256;
        settings.AtlasResolution = 256;
        map.SetSettings(settings);
        EXPECT_FALSE(map.GetSettings().VSM.Enabled);
        EXPECT_FALSE(map.GetSettings().VSM.LocalLights);
        EXPECT_FALSE(map.IsVirtualShadowMapActive());
    }

    TEST_F(ShadowFamilyVisualEvidence, HeightRangeIncludesInteriorPeaksAndRefreshesAfterEdits)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        TerrainData heightmap;
        heightmap.CreateFlat(65, 0.2f);
        EXPECT_FLOAT_EQ(heightmap.GetHeightRange().value().y, 0.2f);
        auto& heights = heightmap.GetHeightData();
        heights[31 * 65 + 29] = 0.9f;
        heightmap.UploadToGPU();
        EXPECT_FLOAT_EQ(heightmap.GetHeightRange().value().y, 0.9f);
        EXPECT_FLOAT_EQ(heightmap.GetHeightRange().value().x, 0.2f);
        heightmap.GetHeightData()[31 * 65 + 29] = 0.1f;
        heightmap.UploadToGPU();
        EXPECT_FLOAT_EQ(heightmap.GetHeightRange().value().y, 0.2f);
        EXPECT_FLOAT_EQ(heightmap.GetHeightRange().value().x, 0.1f);
        const u64 beforeGPUWrite = heightmap.GetHeightRevision();
        heightmap.MarkGPUModified();
        EXPECT_GT(heightmap.GetHeightRevision(), beforeGPUWrite);
        EXPECT_FALSE(heightmap.GetHeightRange().has_value());
        EXPECT_TRUE(heightmap.IsCPUMirrorStale()) << "shadow bounds must not force a sculpt readback";
    }

    TEST_F(ShadowFamilyVisualEvidence, TerrainCastsIntoDirectionalAndLocalVirtualMaps)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Evidence(Family::Terrain);
        PresentationEvidence(Family::Terrain);
    }
    TEST_F(ShadowFamilyVisualEvidence, BothVoxelFormsCastIntoDirectionalAndLocalVirtualMaps)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Evidence(Family::Voxel);
        Evidence(Family::GreedyVoxel);
        PresentationEvidence(Family::Voxel);
        PresentationEvidence(Family::GreedyVoxel);
    }
    TEST_F(ShadowFamilyVisualEvidence, FoliageCardsAndAuthoredMeshesCastIntoDirectionalAndLocalVirtualMaps)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Evidence(Family::FoliageCard);
        Evidence(Family::FoliageMesh);
        PresentationEvidence(Family::FoliageCard);
        PresentationEvidence(Family::FoliageMesh);
    }

    TEST_F(ShadowFamilyVisualEvidence, FarImpostorFoliageCastsIntoDirectionalAndLocalVirtualMaps)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        m_ImpostorEvidence = true;
        Evidence(Family::FoliageMesh);
        if (std::getenv("OLO_VSM_FAMILY_FULL_MATRIX") != nullptr)
            PresentationEvidence(Family::FoliageMesh);
    }

    TEST_F(ShadowFamilyVisualEvidence, OffscreenTerrainStillCastsOntoTheVisibleReceiver)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        AddFamily(Family::Terrain);
        SetLight(0);
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        EditorCamera camera(20.0f, static_cast<f32>(kWidth) / kHeight, 0.1f, 200.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose({ 12, 14, 26 }, 0.0f, 0.45f);
        std::array<ShadowMoment, 2> moments;
        for (u32 mode = 0; mode < 2; ++mode)
        {
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            settings.Enabled = true;
            settings.VSM.Enabled = mode != 0;
            Renderer3D::GetShadowMap().SetSettings(settings);
            const std::string cell = std::string("TerrainOffscreen_GL_Deferred_") + (mode ? "VSM" : "CSM");
            Cast(false);
            const auto lit = Capture(camera, cell + "_lit");
            Cast(true);
            const auto shadow = Capture(camera, cell + "_shadow");
            moments[mode] = Measure(camera, lit, shadow);
            Record(cell, moments[mode]);
            EXPECT_GT(moments[mode].Pixels, 30u);
            EXPECT_FALSE(std::ranges::contains(shadow.EntityIDs, static_cast<i32>(static_cast<u32>(m_Caster))))
                << "caster must actually be outside the main view";
        }
        EXPECT_LT(glm::length(moments[0].Position - moments[1].Position), 1.5);
        GetScene().DestroyEntity(m_Caster);
    }

    TEST_F(ShadowFamilyVisualEvidence, TerrainReceivesVirtualShadowsInsteadOfTheClearedCascade)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Entity floor(static_cast<entt::entity>(static_cast<u32>(m_FloorID)), &GetScene());
        floor.RemoveComponent<MeshComponent>();
        floor.RemoveComponent<MaterialComponent>();
        auto& transform = floor.GetComponent<TransformComponent>();
        transform.Translation = { -20, 0, -20 };
        transform.Scale = { 1, 1, 1 };
        auto& terrain = floor.AddComponent<TerrainComponent>();
        terrain.m_WorldSizeX = terrain.m_WorldSizeZ = 40.0f;
        terrain.m_HeightScale = 8.0f;
        terrain.m_CollisionEnabled = false;
        terrain.m_TerrainData = Ref<TerrainData>::Create();
        terrain.m_TerrainData->CreateFlat(65, 0.0f);
        terrain.m_Material = Ref<TerrainMaterial>::Create();
        m_Caster = GetScene().CreateEntity("Mesh occluder");
        m_Caster.GetComponent<TransformComponent>().Translation = { 0, 3, 0 };
        m_Caster.GetComponent<TransformComponent>().Scale = { 4, 6, 4 };
        auto& mesh = m_Caster.AddComponent<MeshComponent>();
        mesh.m_Primitive = MeshPrimitive::Cube;
        mesh.m_MeshSource = MeshPrimitives::CreateCube()->GetMeshSource();
        m_Caster.AddComponent<MaterialComponent>();
        SetLight(0);
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        EditorCamera camera(45.0f, static_cast<f32>(kWidth) / kHeight, 0.1f, 200.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose({ 4, 14, 26 }, 0.0f, 0.45f);
        std::array<ShadowMoment, 2> moments;
        for (u32 mode = 0; mode < 2; ++mode)
        {
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            settings.Enabled = true;
            settings.VSM.Enabled = mode != 0;
            Renderer3D::GetShadowMap().SetSettings(settings);
            const std::string cell = std::string("TerrainReceiver_GL_Forward_") + (mode ? "VSM" : "CSM");
            Cast(false);
            const auto lit = Capture(camera, cell + "_lit");
            Cast(true);
            const auto shadow = Capture(camera, cell + "_shadow");
            moments[mode] = Measure(camera, lit, shadow);
            Record(cell, moments[mode]);
            EXPECT_GT(moments[mode].Pixels, 30u);
        }
        EXPECT_LT(glm::length(moments[0].Position - moments[1].Position), 1.5);
        EXPECT_GT(moments[1].Mass, moments[0].Mass * 0.15);
        GetScene().DestroyEntity(m_Caster);
    }

    TEST_F(ShadowFamilyVisualEvidence, FamilyScenesRoundTripThroughTheRealAssetPack)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        namespace fs = std::filesystem;
        const bool retainPackage = std::getenv("OLO_VSM_COOK_EVIDENCE") != nullptr;
        const auto editorRoot = fs::path(OLO_TEST_EDITOR_ROOT);
        const auto root = retainPackage ? editorRoot / "assets/tests/visual/ShadowFamilyCooked" : TempDir("vsm-family-pack");
        const auto projectRoot = root / "Authoring";
        fs::create_directories(projectRoot / "Assets/Scenes");
        fs::create_directories(root / "Assets");
        fs::create_directories(root / "Scenes");

        struct ProjectGuard
        {
            Ref<Project> Previous = Project::GetActive();
            Ref<AssetManagerBase> Assets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            ~ProjectGuard()
            {
                Project::Unload();
                if (Previous)
                    Project::NewInMemory(Previous->GetDirectory(), Previous->GetConfig());
                if (Assets)
                    Project::SetAssetManager(Assets);
            }
        } projectGuard;
        ProjectConfig config;
        config.Name = "VSMFamilyEvidence";
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(Project::NewInMemory(projectRoot, config));
        auto editorAssets = Ref<EditorAssetManager>::Create();
        editorAssets->Initialize(false);
        Project::SetAssetManager(editorAssets);
        AssetRegistry registry;
        TArray<AssetHandle> handles;
        std::vector<fs::path> sceneFiles; // StageSceneReferencedContent API boundary
        for (const auto family : { Family::Terrain, Family::Voxel, Family::GreedyVoxel, Family::FoliageCard, Family::FoliageMesh })
        {
            AddFamily(family);
            if (family == Family::GreedyVoxel)
            {
                // This shipped subject uses the supported heightfield-seeded
                // greedy route. VOX1 edits themselves are not persisted yet.
                auto& data = m_Caster.GetComponent<TerrainComponent>().m_TerrainData;
                for (auto& height : data->GetHeightData())
                    height = 0.6f;
                data->UploadToGPU();
            }
            for (u32 light = 0; light < 3; ++light)
            {
                SetLight(light);
                ExportLiveFixture(family, light, "ShadowFamilyCookFixture");
                const auto name = std::string("ShadowFamilyCookFixture_") + FamilyName(family) + "_" + kLightNames[light] + ".olo";
                const auto source = editorRoot / "assets/tests/visual" / name;
                const auto staged = projectRoot / "Assets/Scenes" / name;
                fs::copy_file(source, staged, fs::copy_options::overwrite_existing);
                fs::copy_file(source, root / "Scenes" / name, fs::copy_options::overwrite_existing);
                const auto handle = editorAssets->ImportAsset(staged);
                ASSERT_NE(static_cast<u64>(handle), 0u);
                registry.AddAsset(editorAssets->GetMetadata(handle));
                handles.Add(handle);
                sceneFiles.push_back(staged);
            }
            GetScene().DestroyEntity(m_Caster);
        }
        ASSERT_TRUE(editorAssets->SerializeAssetRegistry()) << "the builder reads the persisted registry";
        AssetPackBuilder::BuildSettings settings;
        settings.m_OutputPath = root / "Assets/AssetPack.olopack";
        settings.m_IncludeScriptModule = false;
        settings.m_IncludeLocalizationFiles = false;
        std::atomic<f32> progress{ 0.0f };
        const auto packed = AssetPackBuilder::BuildFromRegistry(registry, settings, progress);
        ASSERT_TRUE(packed.m_Success) << packed.m_ErrorMessage;
        EXPECT_EQ(packed.m_FailedAssetCount, 0u);
        EXPECT_EQ(packed.m_SceneCount, 15u);
        auto runtimeAssets = Ref<RuntimeAssetManager>::Create(false);
        ASSERT_TRUE(runtimeAssets->LoadAssetPack(settings.m_OutputPath));
        Project::SetAssetManager(runtimeAssets);
        for (const auto handle : handles)
        {
            auto asset = runtimeAssets->GetAsset(handle);
            ASSERT_TRUE(asset);
            ASSERT_EQ(asset->GetAssetType(), AssetType::Scene);
            const auto scene = asset.As<Scene>();
            ASSERT_TRUE(scene);
            EXPECT_EQ(scene->GetAllEntitiesWith<CameraComponent>().size(), 1u);
            EXPECT_EQ(scene->GetAllEntitiesWith<TerrainComponent>().size(), 1u);
            for (const auto entity : scene->GetAllEntitiesWith<TerrainComponent>())
            {
                const auto& terrain = scene->GetAllEntitiesWith<TerrainComponent>().get<TerrainComponent>(entity);
                EXPECT_FALSE(terrain.m_HeightmapPath.empty());
                EXPECT_FALSE(terrain.m_VoxelOverride) << "current scene format does not carry VOX1 edits";
            }
        }
        if (retainPackage)
        {
            sizet count = 0;
            std::string error;
            std::vector<std::string> unresolved; // production staging API boundary
            ASSERT_TRUE(StageSceneReferencedContent(sceneFiles, projectRoot, "Assets", editorRoot, root, count, unresolved, error)) << error;
            EXPECT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());
            for (const char* content : { "shaders", "fonts" })
                fs::copy(editorRoot / "assets" / content, root / "assets" / content,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            const auto runtimeBin = editorRoot.parent_path() / "bin/Release/OloRuntime";
            fs::copy_file(runtimeBin / "OloRuntime.exe", root / "VSMFamilyEvidence.exe", fs::copy_options::overwrite_existing);
            ASSERT_TRUE(StageRuntimeDependencyLibraries(BuildTargetPlatform::Windows, runtimeBin, root, count, error)) << error;
            for (const char* content : { "mono", "Resources/Scripts" })
                if (fs::exists(editorRoot / content))
                {
                    fs::create_directories((root / content).parent_path());
                    fs::copy(editorRoot / content, root / content, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                }
            std::ofstream(root / "game.manifest") << "Game:\n  Name: VSMFamilyEvidence\nStartScene: Scenes/ShadowFamilyCookFixture_Terrain_Sun.olo\nRendering:\n  Is3DMode: true\n";
            RecordUnsupported(Family::Voxel, "Cooked VOX1 edits are not persisted by SceneSerializer; regular-voxel authored volumes cannot ship through the current scene format. Greedy heightfield seeding is verified separately.");
        }
    }
} // namespace OloEngine::Tests
