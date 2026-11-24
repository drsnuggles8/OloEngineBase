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
    TEST(CrowdManagedBinding, ManagedClipAndPhaseWriteReachesOnePlacement)
    {
#if OLO_ENABLE_CSHARP_SCRIPTING
        // Mono's root domain has process lifetime. Run this case by itself
        // with the normal built ScriptCore assembly.
        if (!Env::Get("OLO_CROWD_MANAGED_SMOKE"))
            GTEST_SKIP() << "Set OLO_CROWD_MANAGED_SMOKE=1 for the isolated managed binding check";
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
        auto entity = scene->CreateEntity("Crowd");
        auto& members = entity.AddComponent<InstancedMeshComponent>();
        members.Instances.SetNum(2);
        members.Instances[1].StableID = 1749;
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
        scene->DestroyEntity(entity);
        (void)internal.InvokeMethod(nullptr, method, args, &threw);
        EXPECT_FALSE(threw);
#else
        GTEST_SKIP() << "C# scripting disabled in this build";
#endif
    }
} // namespace OloEngine::Tests
