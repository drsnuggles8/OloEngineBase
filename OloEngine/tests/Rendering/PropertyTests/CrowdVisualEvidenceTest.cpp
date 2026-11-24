// OLO_TEST_LAYER: integration
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "VisualEvidenceGuards.h"
#include "TestRenderBackend.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Crowd/CrowdLod.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Core/Environment.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/ModelImporter.h"

#include <filesystem>
#include <iostream>
#include <stb_image/stb_image_write.h>

namespace OloEngine::Tests
{
    namespace
    {
        struct Frame
        {
            u32 Width = 0, Height = 0;
            std::vector<u8> Pixels;
        };

        u32 ChangedPixels(const Frame& a, const Frame& b)
        {
            if (a.Width != b.Width || a.Height != b.Height || a.Pixels.size() != b.Pixels.size())
            {
                ADD_FAILURE() << "capture dimensions differ";
                return 0;
            }
            u32 changed = 0;
            for (sizet i = 0; i < a.Pixels.size(); i += 4)
                if (std::max({ std::abs(a.Pixels[i] - b.Pixels[i]), std::abs(a.Pixels[i + 1] - b.Pixels[i + 1]),
                               std::abs(a.Pixels[i + 2] - b.Pixels[i + 2]) }) > 12)
                    ++changed;
            return changed;
        }

        void WriteCrowdFrame(const Frame& frame, const std::string& feature, const std::string& cell)
        {
            const std::filesystem::path dir = "assets/tests/visual";
            std::filesystem::create_directories(dir);
            auto prefix = TestRenderBackend::EvidenceName(feature);
            if (prefix.ends_with("_GL_"))
                prefix.replace(prefix.size() - 3, 2, "OpenGL");
            const auto file = dir / (prefix + cell + ".png");
            const int stride = static_cast<int>(frame.Width * 4u);
            ASSERT_GT(frame.Height, 0u);
            ASSERT_EQ(frame.Pixels.size(), static_cast<sizet>(stride) * frame.Height);
            // ReadbackComposite normalizes both RHIs to bottom-up GL order.
            const u8* topRow = frame.Pixels.data() + static_cast<sizet>(frame.Height - 1u) * stride;
            ASSERT_NE(stbi_write_png(file.string().c_str(), frame.Width, frame.Height, 4, topRow, -stride), 0);
        }
    } // namespace

    class CrowdVisualEvidence : public RendererAttachedTest
    {
      protected:
        bool RunsInVulkanProcess() const override
        {
            return true;
        }
        Entity Owner;
        TArray<InstanceData> Placements;
        Frame LastOn;

        void BuildScene() override
        {
            EnableRendering(640, 360);
            auto& scene = GetScene();
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);
            scene.SetLightGizmosVisible(false);
            scene.SetCameraFrustumsVisible(false);
            Entity ground = scene.CreateEntity("Shadow receiver");
            ground.GetComponent<TransformComponent>().Translation.y = -0.5f;
            ground.GetComponent<TransformComponent>().Scale = { 1500.0f, 1.0f, 1500.0f };
            RunInFrame([&]
                       { ground.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource()); });
            Entity sun = scene.CreateEntity("Sun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(-0.8f, -1.0f, -0.35f));
            light.m_Intensity = 3.0f;
            light.m_CastShadows = true;
            Owner = scene.CreateEntity("Crowd");
            ASSERT_NO_FATAL_FAILURE(RunInFrame([&]
                                               {
                const auto model = Ref<AnimatedModel>::Create("assets/models/Fox/Fox.gltf");
                ASSERT_TRUE(model->HasSkeleton());
                ASSERT_GE(model->GetAnimations().size(), 2u);
                ModelImporter::PopulateAnimatedEntity(Owner, model, "assets/models/Fox/Fox.gltf"); }));
            auto& animation = Owner.GetComponent<AnimationStateComponent>();
            animation.m_CurrentClipIndex = 1;
            animation.m_CurrentClip = animation.m_AvailableClips[1];
            animation.m_IsPlaying = true;
            auto& settings = Owner.AddComponent<CrowdAnimationComponent>();
            settings.m_NearPixels = 90.0f;
            settings.m_FarPixels = 32.0f;
            Owner.AddComponent<InstancedMeshComponent>();
            for (u32 row = 0; row < 3; ++row)
                for (u32 col = 0; col < 6; ++col)
                {
                    InstanceData member;
                    const auto x = (static_cast<f32>(col) - 2.5f) * (2.8f + row * 2.0f);
                    member.Transform = glm::translate(glm::mat4(1), glm::vec3(x, 0, -static_cast<f32>(row * row) * 11.0f));
                    member.Transform = glm::rotate(member.Transform, 0.75f + col * 0.18f, glm::vec3(0, 1, 0));
                    member.Transform = glm::scale(member.Transform, glm::vec3(0.02f));
                    member.StableID = Placements.Num() + 1;
                    member.Custom = static_cast<f32>(member.StableID) / 18.0f;
                    Placements.Add(member);
                }
        }

        void Capture(const EditorCamera& camera, Frame& out)
        {
            RunEditorFrames(camera, 5);
            ASSERT_TRUE(ReadbackComposite(out.Pixels, out.Width, out.Height));
        }

        void CheckCell(RenderingPath path, const std::string& cell, u32 width = 640, u32 height = 360, f32 eyeHeight = 4.0f)
        {
            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.Path = path;
            Renderer3D::ApplyRendererSettings();
            ResizeRenderTarget(width, height);
            EditorCamera camera(45.0f, static_cast<f32>(width) / height, 0.05f, 1400.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose({ 0, eyeHeight, 12 }, 0, std::atan2(eyeHeight, 20.0f));
            auto& instances = Owner.GetComponent<InstancedMeshComponent>();
            instances.Instances.Reset();
            Frame off, on;
            ASSERT_NO_FATAL_FAILURE(Capture(camera, off));
            instances.Instances = Placements;
            instances.InvalidateMergedCache();
            ASSERT_NO_FATAL_FAILURE(Capture(camera, on));
            ASSERT_EQ(off.Pixels.size(), on.Pixels.size());
            ASSERT_EQ(on.Width, width);
            ASSERT_EQ(on.Height, height);
            ASSERT_NO_FATAL_FAILURE(WriteCrowdFrame(off, "CrowdOff", cell));
            ASSERT_NO_FATAL_FAILURE(WriteCrowdFrame(on, "Crowd", cell));
            const auto pixels = ChangedPixels(on, off);
            const auto& frame = RendererProfiler::GetInstance().GetLastCompletedFrameData();
            const auto counts = GetScene().ReadCrowdVisibilityCounts();
            std::cout << "[ CROWD ] " << cell << " changedPixels=" << pixels << " of " << width * height
                      << " tierPackets=" << frame.m_AnimatedSubmeshPackets << " visibleNearMidFar="
                      << counts.Visible[0] << "," << counts.Visible[1] << "," << counts.Visible[2] << '\n';
            EXPECT_EQ(frame.m_AnimatedSubmeshPackets, 3u);
            EXPECT_EQ(counts.Members, static_cast<u32>(Placements.Num()));
            EXPECT_GT(counts.Visible[0] + counts.Visible[1] + counts.Visible[2], 0u);
            EXPECT_GT(pixels, 8u);
            if (path == RenderingPath::Deferred)
            {
                const auto* pass = static_cast<SceneRenderPass*>(Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry));
                ASSERT_TRUE(pass && pass->GetGBuffer());
                const auto& buffer = pass->GetGBuffer();
                std::cout << "[ CROWD ] " << cell << " internal=" << buffer->GetWidth() << 'x' << buffer->GetHeight()
                          << " gbufferSamples=" << buffer->GetSampleCount()
                          << " taa=" << Renderer3D::IsEngineTAAWanted()
                          << " fsr2=" << Renderer3D::IsTemporalUpscaleActive() << '\n';
            }
            LastOn = std::move(on);
        }
    };

    TEST_F(CrowdVisualEvidence, Forward)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CheckCell(RenderingPath::Forward, "Forward");
    }
    TEST_F(CrowdVisualEvidence, ForwardPlus)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CheckCell(RenderingPath::ForwardPlus, "ForwardPlus");
    }
    TEST_F(CrowdVisualEvidence, Deferred)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CheckCell(RenderingPath::Deferred, "Deferred");
    }

    TEST_F(CrowdVisualEvidence, OneMemberChangesClipWithoutMovingToAnotherOwner)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& animation = Owner.GetComponent<AnimationStateComponent>();
        animation.m_CurrentTime = 0.19f;
        animation.m_IsPlaying = false;
        auto& members = Owner.GetComponent<InstancedMeshComponent>();
        members.Instances = Placements;
        members.CastShadows = false;
        EditorCamera camera(45.0f, 640.0f / 360.0f, 0.05f, 1400.0f);
        camera.SetViewportSize(640, 360);
        camera.SetPose({ 0, 4, 12 }, 0, std::atan2(4.0f, 20.0f));
        Frame before, after;
        ASSERT_NO_FATAL_FAILURE(Capture(camera, before));
        const u64 identity = members.Instances[2].StableID;
        ASSERT_TRUE(members.SetInstanceAnimation(2, 2, 0.4f));
        ASSERT_NO_FATAL_FAILURE(Capture(camera, after));
        EXPECT_EQ(members.Instances[2].StableID, identity);
        EXPECT_EQ(members.Instances[3].GetCrowdClipIndex(), -1);
        EXPECT_EQ(GetScene().ReadCrowdVisibilityCounts().Members, static_cast<u32>(Placements.Num()));
        EXPECT_EQ(RendererProfiler::GetInstance().GetLastCompletedFrameData().m_AnimatedSubmeshPackets, 6u);
        EXPECT_GT(ChangedPixels(before, after), 4u);
    }

    TEST_F(CrowdVisualEvidence, PatchedMaterialReachesAtlasAndIndividualDraws)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (Owner.HasComponent<MaterialComponent>())
            Owner.RemoveComponent<MaterialComponent>();
        auto& placements = Owner.GetComponent<InstancedMeshComponent>();
        placements.Instances = Placements;
        placements.CastShadows = false;
        auto& animation = Owner.GetComponent<AnimationStateComponent>();
        animation.m_IsPlaying = false;
        animation.m_CurrentTime = 0.2f;
        EditorCamera camera(45.0f, 640.0f / 360.0f, 0.05f, 1400.0f);
        camera.SetViewportSize(640, 360);
        camera.SetPose({ 0, 4, 12 }, 0, std::atan2(4.0f, 20.0f));
        const auto greenPixels = [](const Frame& frame)
        {
            u32 count = 0;
            for (sizet i = 0; i < frame.Pixels.size(); i += 4)
                if (frame.Pixels[i + 1] > frame.Pixels[i] + 20 && frame.Pixels[i + 1] > frame.Pixels[i + 2] + 20)
                    ++count;
            return count;
        };
        Frame control, atlas, individual;
        ASSERT_NO_FATAL_FAILURE(Capture(camera, control));
        const auto* imported = Owner.GetComponent<MeshComponent>().m_MeshSource->GetImportedMaterialPtrForSubmesh(0);
        ASSERT_TRUE(imported);
        MaterialOverride patch;
        patch.MaterialName = imported->GetName();
        patch.OverrideBaseColor = true;
        patch.BaseColor = { 0, 1, 0, 1 };
        Owner.AddComponent<MaterialOverridesComponent>().m_Overrides.Add(patch);
        ASSERT_NO_FATAL_FAILURE(Capture(camera, atlas));
        EXPECT_GT(greenPixels(atlas), greenPixels(control) + 8u);
        EXPECT_EQ(RendererProfiler::GetInstance().GetLastCompletedFrameData().m_AnimatedSubmeshPackets, 3u);
        animation.m_RootMotion = true;
        ASSERT_NO_FATAL_FAILURE(Capture(camera, individual));
        EXPECT_GT(greenPixels(individual), greenPixels(control) + 8u);
        const auto& frame = RendererProfiler::GetInstance().GetLastCompletedFrameData();
        EXPECT_EQ(frame.m_AnimatedSubmeshPackets, static_cast<u32>(Placements.Num()));
        EXPECT_GT(frame.m_AnimatedSubmissionMs, 0.0);
    }

    TEST_F(CrowdVisualEvidence, MixedMaterialsCountEachVisibleMemberOnce)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (Owner.HasComponent<MaterialComponent>())
            Owner.RemoveComponent<MaterialComponent>();
        ASSERT_NO_FATAL_FAILURE(RunInFrame([&]
                                           {
            const std::string path = "SandboxProject/Assets/Models/Horse/Horse.gltf";
            const auto horse = Ref<AnimatedModel>::Create(path);
            ASSERT_TRUE(horse->HasSkeleton());
            ModelImporter::PopulateAnimatedEntity(Owner, horse, path);
            auto source = Owner.GetComponent<MeshComponent>().m_MeshSource;
            ASSERT_EQ(source->GetSubmeshes().Num(), 2);
            TArray<Ref<Material>> materials;
            for (const auto& original : source->GetImportedMaterials())
                materials.Add(original ? Ref<Material>::Create(*original) : nullptr);
            const auto slot = source->GetSubmeshes()[0].m_MaterialIndex;
            ASSERT_LT(slot, static_cast<u32>(materials.Num()));
            ASSERT_TRUE(materials[slot]);
            materials[slot]->SetAlphaMode(AlphaMode::Blend);
            materials[slot]->SetFlag(MaterialFlag::Blend, true);
            source->SetImportedMaterials(std::move(materials)); }));
        auto& batch = Owner.GetComponent<InstancedMeshComponent>();
        batch.Instances.Append(Placements.GetData(), 6);
        batch.InvalidateMergedCache();
        EditorCamera camera(45.0f, 640.0f / 360.0f, 0.05f, 1400.0f);
        camera.SetViewportSize(640, 360);
        camera.SetPose({ 0, 4, 12 }, 0, std::atan2(4.0f, 20.0f));
        RunEditorFrames(camera, 4);
        const auto counts = GetScene().ReadCrowdVisibilityCounts();
        EXPECT_EQ(counts.Members, 6u);
        EXPECT_EQ(counts.Visible[0] + counts.Visible[1] + counts.Visible[2], 6u);
        const auto& frame = RendererProfiler::GetInstance().GetLastCompletedFrameData();
        EXPECT_EQ(frame.m_CrowdMembersSubmitted, 6u);
        EXPECT_EQ(frame.m_AnimatedSubmeshPackets, 9u);
    }

    TEST_F(CrowdVisualEvidence, CommittedKestrelPopulation)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!Env::Get("OLO_CROWD_MATRIX"))
            GTEST_SKIP() << "Set OLO_CROWD_MATRIX=1 to exercise the full KESTREL scene";
        struct RestoreProject
        {
            Ref<Project> Previous = Project::GetActive();
            Ref<AssetManagerBase> PreviousAssets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            Ref<EditorAssetManager> Assets;
            ~RestoreProject()
            {
                if (Assets)
                    Assets->Shutdown();
                Project::RestoreActive(Previous, PreviousAssets);
            }
        } project;
        ASSERT_TRUE(Project::Load("KestrelProject/Kestrel.oloproj"));
        project.Assets = Ref<EditorAssetManager>::Create();
        project.Assets->Initialize(false);
        Project::SetAssetManager(project.Assets);
        auto skiers = Scene::Create();
        ASSERT_NO_FATAL_FAILURE(RunInFrame([&]
                                           { ASSERT_TRUE(SceneSerializer(skiers).Deserialize(Project::GetAssetDirectory() / "Scenes/KestrelCrowd.olo")); }));
        skiers->SetIs3DModeEnabled(true);
        skiers->SetGridVisible(false);
        skiers->SetWorldAxisHelperVisible(false);
        skiers->SetLightGizmosVisible(false);
        skiers->SetCameraFrustumsVisible(false);
        skiers->SetRenderingEnabled(true);
        skiers->OnViewportResize(1920, 1080);
        Renderer3D::OnWindowResize(1920, 1080);
        EditorCamera camera(45, 1920.0f / 1080, 0.01f, 2000.0f);
        camera.SetViewportSize(1920, 1080);
        camera.SetPose({ 0, 65, 0 }, 0, glm::radians(32.0f));
        RunEditorFramesOn(*skiers, camera, 5);
        const auto counts = skiers->ReadCrowdVisibilityCounts();
        EXPECT_EQ(counts.Members, 5000u);
        EXPECT_EQ(counts.Visible[0] + counts.Visible[1] + counts.Visible[2], 5000u);
        EXPECT_GT(*std::min_element(counts.Visible.begin(), counts.Visible.end()), 0u);
        Frame result;
        ASSERT_TRUE(ReadbackComposite(result.Pixels, result.Width, result.Height));
        WriteCrowdFrame(result, "Crowd", "Kestrel5000");
        std::cout << "[ CROWD ] Kestrel5000 near/mid/far=" << counts.Visible[0] << ',' << counts.Visible[1] << ',' << counts.Visible[2] << '\n';
    }

    TEST_F(CrowdVisualEvidence, SettingsAndShadows)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!Env::Get("OLO_CROWD_MATRIX"))
            GTEST_SKIP() << "Set OLO_CROWD_MATRIX=1 to capture the extended settings matrix";
        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.TAAEnabled = true;
        CheckCell(RenderingPath::Deferred, "Deferred_TAA_1080p", 1920, 1080);
        ASSERT_TRUE(Renderer3D::IsEngineTAAWanted());
        auto* pass = static_cast<SceneRenderPass*>(Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry));
        ASSERT_TRUE(pass && pass->GetGBuffer());
        const auto& gb = pass->GetGBuffer();
        std::vector<f32> velocity;
        ReadbackRgbaFloat(gb->GetColorAttachmentHandle(GBuffer::Velocity), gb->GetWidth(), gb->GetHeight(), velocity);
        f32 peak = 0.0f;
        for (sizet i = 0; i < velocity.size(); i += 4)
            peak = std::max(peak, std::max(std::abs(velocity[i]), std::abs(velocity[i + 1])));
        std::cout << "[ CROWD ] velocity peak=" << peak << '\n';
        EXPECT_GT(peak, 1.0e-5f);

        pp.TAAEnabled = false;
        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Spatial;
        CheckCell(RenderingPath::Deferred, "Deferred_Spatial");
        EXPECT_EQ(Renderer3D::GetUpscaleResolution().Result.Resolved, TemporalUpscalePolicy::ResolvedUpscaler::Spatial);
        pp.Technique = UpscalerTechnique::Temporal;
        CheckCell(RenderingPath::Deferred, "Deferred_TemporalRequest");
        const auto& resolved = Renderer3D::GetUpscaleResolution();
        std::cout << "[ CROWD ] temporalUpscaleActive=" << Renderer3D::IsTemporalUpscaleActive()
                  << " deviceStatus=" << ToString(resolved.UpscalerStatus) << '\n';
        if (resolved.UpscalerStatus == TemporalUpscalerStatus::Available)
            EXPECT_TRUE(Renderer3D::IsTemporalUpscaleActive());
        else
        {
            EXPECT_TRUE(resolved.UpscalerStatus == TemporalUpscalerStatus::BackendUnsupported ||
                        resolved.UpscalerStatus == TemporalUpscalerStatus::DeviceUnsupported ||
                        resolved.UpscalerStatus == TemporalUpscalerStatus::NotCompiledIn);
            EXPECT_FALSE(Renderer3D::IsTemporalUpscaleActive());
            EXPECT_EQ(resolved.Result.Resolved, TemporalUpscalePolicy::ResolvedUpscaler::Spatial);
        }
        pp.Upscale = UpscaleMode::Off;

        auto& renderer = Renderer3D::GetRendererSettings();
        renderer.Deferred.MSAASampleCount = std::min(4u, Renderer3D::GetMaxMSAASamples());
        CheckCell(RenderingPath::Deferred, "Deferred_MSAA");
        const auto* msaaPass = static_cast<SceneRenderPass*>(Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry));
        ASSERT_TRUE(msaaPass && msaaPass->GetGBuffer());
        EXPECT_GT(msaaPass->GetGBuffer()->GetSampleCount(), 1u);
        renderer.Deferred.MSAASampleCount = 1;

        Owner.GetComponent<AnimationStateComponent>().m_IsPlaying = false;
        auto& instances = Owner.GetComponent<InstancedMeshComponent>();
        const auto nonShadowRayInstances = []
        {
            const auto& gpuScene = Renderer3D::GetGPUScene();
            u32 count = 0;
            for (u32 slot = 0; slot < gpuScene.GetInstanceSlotCount(); ++slot)
            {
                const auto* record = gpuScene.GetLiveInstanceRecordBySlot(slot);
                if (record && !(RayTracing::PackInstanceMask(record->VisibilityMask) & RayTracing::kInstanceMaskShadowCaster))
                {
                    EXPECT_NE(RayTracing::PackInstanceMask(record->VisibilityMask), 0u);
                    ++count;
                }
            }
            return count;
        };
        for (const bool vsm : { false, true })
        {
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            settings.VSM.Enabled = vsm;
            Renderer3D::GetShadowMap().SetSettings(settings);
            const std::string name = vsm ? "Deferred_VSM" : "Deferred_CSM";
            instances.CastShadows = false;
            ASSERT_NO_FATAL_FAILURE(CheckCell(RenderingPath::Deferred, name + "_NoCasters"));
            const auto withoutShadows = nonShadowRayInstances();
            Frame unshadowed = LastOn;
            instances.CastShadows = true;
            ASSERT_NO_FATAL_FAILURE(CheckCell(RenderingPath::Deferred, name));
            EXPECT_EQ(withoutShadows, nonShadowRayInstances() + static_cast<u32>(Placements.Num()));
            const u32 pixels = ChangedPixels(LastOn, unshadowed);
            const auto& shadowMap = Renderer3D::GetShadowMap();
            EXPECT_EQ(shadowMap.IsVirtualShadowMapActive(), vsm);
            std::cout << "[ CROWD ] " << name << " shadowPixels=" << pixels
                      << " vsmActive=" << shadowMap.IsVirtualShadowMapActive()
                      << " residentPages=" << shadowMap.GetVirtualShadowMap().GetStatistics().PagesResident << '\n';
            EXPECT_GT(pixels, 8u);
        }
    }

    TEST_F(CrowdVisualEvidence, EachTierAnimatesAtAFixedCamera)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!Env::Get("OLO_CROWD_MATRIX"))
            GTEST_SKIP() << "Set OLO_CROWD_MATRIX=1 to capture the tier phase comparisons";
        auto& batch = Owner.GetComponent<InstancedMeshComponent>();
        batch.Instances.Append(Placements.GetData(), 6);
        batch.InvalidateMergedCache();
        batch.CastShadows = false;
        auto& animation = Owner.GetComponent<AnimationStateComponent>();
        animation.m_IsPlaying = false;
        auto& lod = Owner.GetComponent<CrowdAnimationComponent>();
        ResizeRenderTarget(960, 540);
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        EditorCamera camera(45.0f, 960.0f / 540.0f, 0.05f, 1400.0f);
        camera.SetViewportSize(960, 540);

        // Two tiers at each handover, photographed from exactly the same
        // camera at the same pair of clip times. Adjacent sweep frames also
        // include the camera movement and any shadow cascade change.
        struct Cell
        {
            const char* Name;
            u32 Tier;
            f32 Height;
        };
        Frame previousTier;
        for (const auto& cell : std::array<Cell, 4>{ { { "Near", 0, 65.0f }, { "Mid", 1, 65.0f }, { "MidAtFar", 1, 180.0f }, { "Far", 2, 180.0f } } })
        {
            lod.m_NearPixels = cell.Tier == 0 ? 1.0f : 4096.0f;
            lod.m_FarPixels = cell.Tier == 2 ? 4096.0f : 1.0f;
            camera.SetPose({ 0, cell.Height, 30 }, 0, std::atan2(cell.Height, 30.0f));
            Frame a, repeat, b;
            animation.m_CurrentTime = 0.06f;
            ASSERT_NO_FATAL_FAILURE(Capture(camera, a));
            ASSERT_NO_FATAL_FAILURE(Capture(camera, repeat));
            animation.m_CurrentTime = 0.38f;
            ASSERT_NO_FATAL_FAILURE(Capture(camera, b));
            const auto counts = GetScene().ReadCrowdVisibilityCounts();
            ASSERT_EQ(counts.Visible[cell.Tier], 6u);
            const u32 noise = ChangedPixels(a, repeat);
            const u32 motion = ChangedPixels(a, b);
            const f64 noiseFloor = VisualEvidence::Rgba8Rmse(a.Pixels, repeat.Pixels);
            VisualEvidence::ExpectCapturesAreDistinct({ a.Pixels, b.Pixels }, { "phase A", "phase B" }, noiseFloor);
            std::cout << "[ CROWD ] " << cell.Name << " motionPixels=" << motion << " repeatPixels=" << noise;
            if (std::string_view(cell.Name) == "Mid" || cell.Tier == 2)
                std::cout << " samePoseTierDifference=" << ChangedPixels(previousTier, a);
            std::cout << '\n';
            WriteCrowdFrame(a, "Crowd", std::string(cell.Name) + "PhaseA");
            WriteCrowdFrame(b, "Crowd", std::string(cell.Name) + "PhaseB");
            previousTier = std::move(a);
        }
    }

    TEST_F(CrowdVisualEvidence, StrategyDistanceSweep)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!Env::Get("OLO_CROWD_MATRIX"))
            GTEST_SKIP() << "Set OLO_CROWD_MATRIX=1 to capture the strategy-camera sweep";
        auto& lod = Owner.GetComponent<CrowdAnimationComponent>();
        lod.m_NearPixels = 30.0f;
        lod.m_FarPixels = 12.0f;
        ResizeRenderTarget(960, 540);
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        auto& animation = Owner.GetComponent<AnimationStateComponent>();
        animation.m_CurrentTime = 0.27f;
        animation.m_IsPlaying = false;
        auto& members = Owner.GetComponent<InstancedMeshComponent>();
        members.Instances = Placements;
        members.InvalidateMergedCache();

        EditorCamera camera(45.0f, 960.0f / 540.0f, 0.05f, 1400.0f);
        camera.SetViewportSize(960, 540);
        // One uninterrupted camera move keeps the GPU history alive. Preserve
        // both frames of the first transition of each kind, in both directions.
        std::array<u32, 3> previousCounts{};
        Frame previous;
        i32 previousHeight = 50;
        for (int direction : { 1, -1 })
        {
            std::array<bool, 2> seen{};
            for (int step = 0; step <= 90; ++step)
            {
                const i32 height = direction > 0 ? 50 + step * 5 : 500 - step * 5;
                // Aim at the centre row of the test footprint. A row leaving
                // the frustum would otherwise mimic a LOD count change.
                camera.SetPose({ 0, static_cast<f32>(height), 12 }, 0, std::atan2(static_cast<f32>(height), 34.0f));
                RunEditorFrames(camera, 1);
                Frame frame;
                ASSERT_TRUE(ReadbackComposite(frame.Pixels, frame.Width, frame.Height));
                const auto counts = GetScene().ReadCrowdVisibilityCounts().Visible;
                ASSERT_EQ(counts[0] + counts[1] + counts[2], static_cast<u32>(Placements.Num()));
                if (step == 0 || step == 90)
                    WriteCrowdFrame(frame, "Crowd", "Strategy" + std::to_string(height));
                for (u32 edge = 0; edge < 2; ++edge)
                {
                    const u32 tier = edge == 0 ? 0 : 2;
                    const u32 otherTier = 2 - tier;
                    const bool exchangedWithMid = counts[otherTier] == previousCounts[otherTier] &&
                                                  counts[1] + counts[tier] == previousCounts[1] + previousCounts[tier];
                    if (step != 0 && !seen[edge] && exchangedWithMid && counts[tier] != previousCounts[tier])
                    {
                        const std::string label = std::string(direction > 0 ? "Away" : "Toward") + (edge == 0 ? "NearMid" : "MidFar");
                        WriteCrowdFrame(previous, "Crowd", label + "Before");
                        WriteCrowdFrame(frame, "Crowd", label + "After");
                        std::cout << "[ CROWD ] " << label << " heights=" << previousHeight << "," << height
                                  << " tierCount=" << previousCounts[tier] << "->" << counts[tier]
                                  << " changedPixels=" << ChangedPixels(previous, frame) << '\n';
                        seen[edge] = true;
                    }
                }
                previous = std::move(frame);
                previousCounts = counts;
                previousHeight = height;
            }
            EXPECT_TRUE(seen[0] && seen[1]) << "camera sweep missed a LOD crossing";
        }
    }
} // namespace OloEngine::Tests
