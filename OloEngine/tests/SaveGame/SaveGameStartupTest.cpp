// OLO_TEST_LAYER: integration
#include "OloEnginePCH.h"
#include "TestTempDir.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace OloEngine;

namespace
{
    void VerifySynchronousStartupLoad(const std::filesystem::path& projectDir)
    {
        ApplicationSpecification spec;
        spec.Name = "Checkpoint startup test";
        spec.IsHeadless = true;
        spec.WorkingDirectory = OLO_TEST_EDITOR_ROOT;
        Application app(spec);
        ProjectConfig config;
        config.AssetDirectory = "Assets";
        Project::NewInMemory(projectDir, config);
        const auto assets = projectDir / "Assets";
        std::filesystem::create_directories(assets);
        std::filesystem::create_directories(projectDir / "Saves");
        std::ofstream(assets / "Loader.lua") << R"(
            return {
                OnCreate = function(id)
                    loaderCalls = (loaderCalls or 0) + 1
                    assert(loaderCalls == 1)
                    assert(SaveGame.Load("startup") == 0)
                    assert(loaderCalls == 1)
                    entity_utils.set_translation(id, vec3.new(7, 0, 0))
                end,
                OnUpdate = function(id) entity_utils.set_translation(id, vec3.new(8, 0, 0)) end
            })";
        std::ofstream(assets / "Observer.lua") << R"(
            return {
                OnCreate = function(id) entity_utils.set_translation(id, vec3.new(1, 0, 0)) end,
                OnUpdate = function(id) entity_utils.set_translation(id, vec3.new(2, 0, 0)) end
            })";
        auto scene = Scene::Create();
        auto addScript = [&](const char* tag, const char* file)
        {
            auto e = scene->CreateEntity(tag);
            e.AddComponent<LuaScriptComponent>().ScriptFile = file;
            return e.GetUUID();
        };
        const UUID loader = addScript("Loader", "Loader.lua");
        const UUID observer = addScript("Before", "Observer.lua");
        const UUID savedOnly = addScript("Saved only", "Observer.lua");
        const auto payload = SaveGameSerializer::CaptureSceneState(*scene);
        SaveGameHeader header;
        header.EntityCount = 3;
        header.PayloadUncompressedSize = payload.size();
        ASSERT_TRUE(SaveGameFile::Write(projectDir / "Saves/startup.olosave", header, {}, {}, payload));
        scene->DestroyEntity(scene->GetEntityByUUID(savedOnly));
        scene->GetEntityByUUID(loader).GetComponent<LuaScriptComponent>().ScriptFile = "./Loader.lua";

        scene->OnRuntimeStart();
        const auto x = [&](UUID id)
        {
            const auto e = scene->TryGetEntityWithUUID(id);
            return e ? e->GetComponent<TransformComponent>().Translation.x : -1.0f;
        };
        EXPECT_FLOAT_EQ(x(loader), 7.0f);
        EXPECT_FLOAT_EQ(x(observer), 1.0f);
        EXPECT_FLOAT_EQ(x(savedOnly), 1.0f);
        scene->OnUpdateRuntime(Timestep{ 1.0f / 60.0f });
        EXPECT_FLOAT_EQ(x(loader), 8.0f);
        EXPECT_FLOAT_EQ(x(observer), 2.0f);
        EXPECT_FLOAT_EQ(x(savedOnly), 2.0f);
        scene->OnRuntimeStop();
    }
} // namespace

TEST(SaveGameStartupTest, SynchronousLoadRecoversScriptInstancesAndShutsDownCleanly)
{
    // Application owns process-wide scripting, physics and scheduler state.
    // Exercise its real startup and shutdown in a fresh process.
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        const auto project = OloEngine::Tests::TempDir("startup-project");
        VerifySynchronousStartupLoad(project);
        std::filesystem::remove_all(project);
        std::_Exit(::testing::Test::HasFailure() ? 1 : 0); }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}
