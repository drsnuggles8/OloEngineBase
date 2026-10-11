#include "OloEnginePCH.h"

// OLO_TEST_LAYER: cullinglod
// =============================================================================
// GroomViewCullTest — a coat no view can see is not deformed or drawn (#1259).
//
// Scene::PublishGroomStrandRequests used to deform, simulate and submit every
// coat every frame. With the acceptance scene's 41-fox herd behind the camera
// that was ~80 ms of CPU and ~65 ms of GPU root frames for coats that drew
// nothing. A coat is now submitted only when its padded bounds reach the
// culling camera's frustum, or, for a coat that casts scene shadows, when its
// shadow can: swept along a shadowing sun, or inside a shadowing local light
// whose range reaches the view.
//
// Each case reads what was SUBMITTED this frame (Renderer3D::
// GetGroomStrandRequests), which is exactly what the cull gates.
// =============================================================================

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 320u;
        constexpr u32 kHeight = 240u;

        Ref<GroomAsset> BuildSmallCoat()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;
            for (u32 s = 0u; s < 200u; ++s)
            {
                const f32 a = 0.0314159f * static_cast<f32>(s);
                const glm::vec3 root(0.3f * std::cos(a), 0.0f, 0.3f * std::sin(a));
                const std::vector<glm::vec3> points{ root, root + glm::vec3(0.0f, 0.15f, 0.0f),
                                                     root + glm::vec3(0.0f, 0.3f, 0.0f) };
                const std::vector<f32> widths{ 0.004f, 0.003f, 0.002f };
                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { static_cast<f32>(s) / 200.0f, 0.5f };
                input.GroupId = group;
                input.IsGuide = (s % 20u) == 0u;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }
            builder.SetName("GroomViewCullCoat");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            return groom;
        }
    } // namespace

    class GroomViewCullTest : public RendererAttachedTest
    {
      protected:
        Entity m_InView;
        Entity m_Behind;
        Entity m_BehindCasterIntoView;
        Entity m_BehindCasterAway;

        void BuildScene() override
        {
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                const fs::path projectDir = TempDir("project");
                std::error_code ec;
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec);
                {
                    std::ofstream proj(projectDir / "Cull.oloproj");
                    proj << "Project:\n  Name: GroomViewCull\n  StartScene: \"\"\n  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Cull.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // The sun shines toward -Z, the camera's view direction, so a
            // caster behind the camera throws its shadow forward into view.
            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.0f, -0.4f, -1.0f));
            dl.m_CastShadows = true;
            dl.m_MaxShadowDistance = 200.0f;

            const AssetHandle coat = AssetManager::AddMemoryOnlyAsset<GroomAsset>(BuildSmallCoat());
            ASSERT_NE(static_cast<u64>(coat), 0u);
            const auto makeCoat = [&scene, coat](const char* name, glm::vec3 at, bool castsShadow)
            {
                Entity e = scene.CreateEntity(name);
                e.GetComponent<TransformComponent>().Translation = at;
                auto& gc = e.AddComponent<GroomComponent>();
                gc.m_Groom = coat;
                gc.m_ShowPreview = false;
                gc.m_RenderStrands = true;
                gc.m_MaxRenderStrands = 200;
                if (castsShadow)
                {
                    auto& shadow = e.AddComponent<GroomSceneShadowComponent>();
                    shadow.m_CastShadows = true;
                }
                return e;
            };
            // The camera stands at z = 4 looking toward -Z.
            m_InView = makeCoat("InView", { 0.0f, 0.0f, 0.0f }, false);
            m_Behind = makeCoat("Behind", { 0.0f, 0.0f, 14.0f }, false);
            m_BehindCasterIntoView = makeCoat("BehindCaster", { 2.0f, 2.0f, 14.0f }, true);
            // Behind, casting, and far off to the side: its sun sweep runs
            // parallel to the view and never enters it.
            m_BehindCasterAway = makeCoat("BehindCasterAway", { 400.0f, 0.0f, 14.0f }, true);
        }

        [[nodiscard]] static EditorCamera Camera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 1.0f, 4.0f), 0.0f, glm::radians(5.0f));
            return camera;
        }

        [[nodiscard]] static bool Submitted(Entity entity)
        {
            const i32 id = static_cast<i32>(std::to_underlying(static_cast<entt::entity>(entity)));
            for (const auto& request : Renderer3D::GetGroomStrandRequests())
            {
                if (request.EntityID == id)
                    return true;
            }
            return false;
        }
    };

    // In view: submitted. Behind the camera with no shadow: not. Behind but
    // casting into view: submitted. Behind and casting away: not.
    TEST_F(GroomViewCullTest, OnlyCoatsThatReachTheViewAreSubmitted)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(Renderer3D::IsFrustumCullingEnabled());
        RunEditorFrames(Camera(), 2);
        EXPECT_TRUE(Submitted(m_InView));
        EXPECT_FALSE(Submitted(m_Behind)) << "a coat behind the camera with no shadow was still deformed and drawn";
        EXPECT_TRUE(Submitted(m_BehindCasterIntoView)) << "a caster whose shadow falls into view was culled";
        EXPECT_FALSE(Submitted(m_BehindCasterAway));
    }

    // Control arm: with frustum culling off every coat is submitted, so the
    // case above measures the cull and not some other reason a coat drops out.
    TEST_F(GroomViewCullTest, WithFrustumCullingOffEveryCoatIsSubmitted)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct Restore
        {
            ~Restore()
            {
                Renderer3D::EnableFrustumCulling(true);
            }
        } restore;
        Renderer3D::EnableFrustumCulling(false);
        RunEditorFrames(Camera(), 2);
        EXPECT_TRUE(Submitted(m_InView));
        EXPECT_TRUE(Submitted(m_Behind));
        EXPECT_TRUE(Submitted(m_BehindCasterIntoView));
        EXPECT_TRUE(Submitted(m_BehindCasterAway));
    }
} // namespace OloEngine::Tests
