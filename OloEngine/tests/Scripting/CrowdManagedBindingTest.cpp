// OLO_TEST_LAYER: integration
#include "OloEnginePCH.h"
#include "OloEngine/Core/Environment.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include <gtest/gtest.h>
#include <filesystem>

namespace OloEngine::Tests
{
#if OLO_ENABLE_CSHARP_SCRIPTING
    // Mono's root domain has process lifetime. Each opt-in check runs in
    // its own process with the normal built ScriptCore assembly.
    static void CheckManagedBinding(void (*check)(Scene&, ScriptClass&))
    {
        struct RestoreWorkingDirectory
        {
            std::filesystem::path Original = std::filesystem::current_path();
            ~RestoreWorkingDirectory()
            {
                std::error_code ignored;
                std::filesystem::current_path(Original, ignored);
            }
        } workingDirectory;
        std::filesystem::current_path(OLO_TEST_EDITOR_ROOT);
        ASSERT_TRUE(std::filesystem::is_regular_file("Resources/Scripts/OloEngine-ScriptCore.dll"));
        auto scene = Scene::Create();
        ScriptEngine::Init(false);
        struct Shutdown
        {
            ~Shutdown()
            {
                ScriptEngine::OnRuntimeStop();
                ScriptEngine::Shutdown();
            }
        } shutdown;
        ASSERT_NE(ScriptEngine::GetCoreAssemblyImage(), nullptr);
        ScriptEngine::OnRuntimeStart(scene.Raw());
        ScriptClass internal("OloEngine", "InternalCalls", true);
        check(*scene, internal);
    }

    static void CheckCrowdAnimation(Scene& scene, ScriptClass& internal)
    {
        auto entity = scene.CreateEntity("Crowd");
        auto& members = entity.AddComponent<InstancedMeshComponent>();
        members.Instances.SetNum(2);
        members.Instances[1].StableID = 1749;
        auto* method = internal.GetMethod("InstancedMeshComponent_SetInstanceAnimation", 4);
        ASSERT_TRUE(ScriptClass::IsMethodStatic(method));
        u64 owner = static_cast<u64>(entity.GetUUID());
        i32 index = 1, clip = 2;
        f32 phase = 0.375f;
        void* args[] = { &owner, &index, &clip, &phase };
        bool threw = false;
        (void)internal.InvokeMethod(nullptr, method, args, &threw);
        EXPECT_FALSE(threw);
        EXPECT_EQ(members.Instances[1].StableID, 1749u);
        EXPECT_EQ(members.Instances[1].GetCrowdClipIndex(), clip);
        EXPECT_FLOAT_EQ(members.Instances[1].Custom, phase);
        EXPECT_EQ(members.Instances[0].GetCrowdClipIndex(), -1);
        scene.DestroyEntity(entity);
        (void)internal.InvokeMethod(nullptr, method, args, &threw);
        EXPECT_FALSE(threw);
    }

    static void CheckWorldNavTarget(Scene& scene, ScriptClass& internal)
    {
        Entity rig = scene.CreateEntity("Rig");
        Entity target = scene.CreateEntity("Target");
        Entity agent = scene.CreateEntity("Agent");
        auto& nav = agent.AddComponent<NavAgentComponent>();
        target.SetParent(rig);
        target.GetComponent<TransformComponent>().Translation = { 1, 0, 0 };
        scene.PropagateWorldTransforms();
        rig.GetComponent<TransformComponent>().Translation = { 3, 0, 2 };

        u64 targetId = target.GetUUID(), agentId = agent.GetUUID();
        glm::vec3 world{};
        void* getArgs[]{ &targetId, &world };
        void* setArgs[]{ &agentId, &world };
        auto* get = internal.GetMethod("Entity_GetWorldTranslation", 2);
        auto* set = internal.GetMethod("NavAgentComponent_SetTargetPosition", 2);
        ASSERT_TRUE(ScriptClass::IsMethodStatic(get));
        ASSERT_TRUE(ScriptClass::IsMethodStatic(set));
        bool threw = false;
        (void)internal.InvokeMethod(nullptr, get, getArgs, &threw);
        ASSERT_FALSE(threw);
        (void)internal.InvokeMethod(nullptr, set, setArgs, &threw);
        ASSERT_FALSE(threw);
        EXPECT_TRUE(nav.m_HasTarget);
        EXPECT_NEAR(glm::distance(nav.m_TargetPosition, glm::vec3(4, 0, 2)), 0.0f, 1.0e-5f);
    }
#endif

    TEST(CrowdManagedBinding, ManagedClipAndPhaseWriteReachesOnePlacement)
    {
#if OLO_ENABLE_CSHARP_SCRIPTING
        if (!Env::Get("OLO_CROWD_MANAGED_SMOKE"))
            GTEST_SKIP() << "Set OLO_CROWD_MANAGED_SMOKE=1 for the isolated managed binding check";
        CheckManagedBinding(CheckCrowdAnimation);
#else
        GTEST_SKIP() << "C# scripting disabled in this build";
#endif
    }

    TEST(NavigationManagedBinding, WorldTranslationFeedsTheCurrentParentedPositionToNavTarget)
    {
#if OLO_ENABLE_CSHARP_SCRIPTING
        if (!Env::Get("OLO_NAV_MANAGED_SMOKE"))
            GTEST_SKIP() << "Set OLO_NAV_MANAGED_SMOKE=1 for the isolated navigation binding check";
        CheckManagedBinding(CheckWorldNavTarget);
#else
        GTEST_SKIP() << "C# scripting disabled in this build";
#endif
    }
} // namespace OloEngine::Tests
