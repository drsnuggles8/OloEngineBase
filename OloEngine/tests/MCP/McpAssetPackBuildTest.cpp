// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "MCP/McpAssetPackBuild.h"
#include "MCP/McpToolsCommon.h"
#include "TestTempDir.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/Entity.h"
#include "UndoRedo/EditorCommand.h"
#include <fstream>
#include <thread>

using namespace OloEngine;
namespace Pack = OloEngine::MCP::AssetPackBuild;

TEST(McpAssetPackBuild, RejectsTraversalAbsolutePathsAndFinalSymlinkEscape)
{
    namespace fs = std::filesystem;
    const auto root = Tests::TempDir("project");
    const auto outside = Tests::TempDir("outside");
    fs::create_directories(root);
    fs::create_directories(outside);
    fs::path resolved;
    for (const char* bad : { "../escape.olopack", "nested/../escape.olopack", "nested\\..\\escape.olopack", "/tmp/escape.olopack", "C:\\escape.olopack", "//server/share/escape.olopack", "Assets/pack.olopack:stream", "Assets/pack.txt", "" })
        EXPECT_TRUE(Pack::ResolveOutput(root, bad, resolved).has_value()) << bad;
    ASSERT_FALSE(Pack::ResolveOutput(root, "Builds/test.olopack", resolved));
    EXPECT_EQ(resolved, fs::weakly_canonical(root / "Builds/test.olopack"));
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "escape", ec);
    if (ec)
        GTEST_SKIP() << "Creating symlinks requires Windows developer mode or symlink privilege: " << ec.message();
    EXPECT_TRUE(Pack::ResolveOutput(root, "escape/test.olopack", resolved));
    std::ofstream(outside / "target.olopack") << "existing";
    fs::create_symlink(outside / "target.olopack", root / "final.olopack", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(Pack::ResolveOutput(root, "final.olopack", resolved));
}

TEST(McpAssetPackBuild, RecordsReportTheSerializersCookedMetadataAndRuntimeScope)
{
    AssetPackBuilder::BuildResult built;
    built.m_Success = true;
    built.m_AssetCount = 1;
    built.m_Records.Add(AssetPackBuilder::AssetRecord{ AssetHandle(123), AssetType::Texture2D,
                                                       FString("Textures/hash.png"), 28, 1024, static_cast<u32>(ImageFormat::BC7), true, 64, 64 });
    const auto result = Pack::ResultJson(built);
    EXPECT_EQ(result["assets"][0]["texture"]["format"], "BC7");
    EXPECT_TRUE(result["assets"][0]["texture"]["sRGB"].get<bool>());
    EXPECT_EQ(result["assets"][0]["handle"], "123");
    EXPECT_FALSE(result["runtime"]["launched"].get<bool>());
    EXPECT_FALSE(result["runtime"]["reason"].get<std::string>().empty());
}

namespace
{
    class PackHost : public Automation::IAutomationHost
    {
      public:
        MCP::EditorMcpContext Editor;
        const MCP::EditorMcpContext& Context() const override
        {
            return Editor;
        }
        bool IsCurrentCallCancelled() const override
        {
            return false;
        }
        bool PublishArtifact(Automation::AutomationArtifact) override
        {
            return false;
        }

      protected:
        MCP::Json MarshalReadOnMainThread(const std::function<MCP::Json()>& job, std::chrono::milliseconds) override
        {
            return job();
        }
        void EmitProgressUpdate(f64, f64, const std::string&) const override {}
    };
} // namespace

TEST(McpAssetPackBuild, RealCommandEnforcesConsentSchemaAndErrorShapes)
{
    Project::Unload();
    Automation::AutomationRegistry registry;
    MCP::RegisterAssetTools(registry);
    PackHost host;
    const auto denied = registry.Invoke(host, "olo_asset_pack_build", { { "output", "Assets/Test.olopack" } });
    EXPECT_EQ(denied.Outcome, Automation::AutomationInvocation::Status::WriteConsentWithheld);
    const auto malformed = registry.Invoke(host, "olo_asset_pack_build", { { "compress", "true" } }, Automation::AutomationWriteConsent::Granted);
    EXPECT_EQ(malformed.Outcome, Automation::AutomationInvocation::Status::InvalidArguments);
    const auto noProject = registry.Invoke(host, "olo_asset_pack_build", { { "output", "Assets/Test.olopack" } }, Automation::AutomationWriteConsent::Granted);
    ASSERT_TRUE(noProject.Ran());
    EXPECT_TRUE(noProject.Result.IsError);
    EXPECT_NE(noProject.Result.Content.dump().find("No active project"), std::string::npos);
    const auto unknown = registry.Invoke(host, "olo_asset_pack_build", { { "operationId", "unknown" } }, Automation::AutomationWriteConsent::Granted);
    ASSERT_TRUE(unknown.Ran());
    EXPECT_TRUE(unknown.Result.IsError);
}

TEST(McpAssetPackBuild, RealBuildAutoSavesPollsAndReturnsWarningsWithBothCompressionSettings)
{
    namespace fs = std::filesystem;
    const auto root = Tests::TempDir("real-pack");
    fs::create_directories(root / "Assets");
    ProjectConfig config;
    config.AssetDirectory = "Assets";
    auto project = Project::NewInMemory(root, config);
    auto manager = Ref<EditorAssetManager>::Create();
    manager->Initialize(false);
    Project::SetAssetManager(manager);
    struct Cleanup
    {
        Ref<EditorAssetManager> Manager;
        ~Cleanup()
        {
            Manager->Shutdown();
            Project::Unload();
        }
    } cleanup{ manager };
    const auto profilePath = root / "Assets/DogNose.oloskin";
    fs::copy_file(fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject/Assets/Materials/DogNose.oloskin", profilePath, fs::copy_options::overwrite_existing);
    const auto handle = manager->ImportAsset(profilePath);
    ASSERT_NE(static_cast<u64>(handle), 0);
    AssetMetadata missing;
    missing.Handle = AssetHandle(990607);
    missing.Type = AssetType::SkinProfile;
    missing.FilePath = "Assets/Missing.oloskin";
    manager->SetMetadata(missing.Handle, missing);
    auto scene = Ref<Scene>::Create();
    scene->SetName("PackSavedScene");
    CommandHistory history;
    PackHost host;
    const auto scenePath = root / "Assets/PackSavedScene.olo";
    SceneSerializer(scene).Serialize(scenePath.string());
    const auto sceneHandle = manager->ImportAsset(scenePath);
    ASSERT_TRUE(manager->GetAsset(sceneHandle)); // Deliberately cache the earlier scene.
    host.Editor.SceneDocument.Capture = [&]
    { return Automation::CaptureSceneDocument(scene, scenePath); };
    host.Editor.SceneDocument.Install = [](const auto&) {};
    host.Editor.SceneDocument.AssetDirectory = [&]
    { return root / "Assets"; };
    host.Editor.GetCommandHistory = [&]
    { return &history; };
    host.Editor.AcquireAssetPackBuildLease = [](std::function<void()>)
    { return std::static_pointer_cast<void>(std::make_shared<int>(0)); };
    Automation::AutomationRegistry registry;
    MCP::RegisterAssetTools(registry);
    PackHost headless;
    const auto unavailable = registry.Invoke(headless, "olo_asset_pack_build", { { "output", "headless.olopack" } }, Automation::AutomationWriteConsent::Granted);
    ASSERT_TRUE(unavailable.Ran());
    EXPECT_TRUE(unavailable.Result.IsError);
    EXPECT_NE(unavailable.Result.Content.dump().find("unavailable in headless attach"), std::string::npos);
    for (bool compress : { false, true })
    {
        const std::string marker = compress ? "FreshCompressedScene" : "FreshPlainScene";
        (void)scene->CreateEntity(marker);
        const auto output = compress ? "compressed.olopack" : "plain.olopack";
        auto start = registry.Invoke(host, "olo_asset_pack_build", { { "output", output }, { "compress", compress } }, Automation::AutomationWriteConsent::Granted);
        ASSERT_TRUE(start.Ran());
        ASSERT_FALSE(start.Result.IsError) << start.Result.Content.dump();
        ASSERT_TRUE(fs::exists(scenePath));
        const auto operation = start.Result.StructuredContent["operationId"];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        MCP::Json completed;
        do
        {
            auto poll = registry.Invoke(host, "olo_asset_pack_build", { { "operationId", operation } }, Automation::AutomationWriteConsent::Granted);
            ASSERT_TRUE(poll.Ran());
            ASSERT_FALSE(poll.Result.IsError);
            completed = poll.Result.StructuredContent;
            if (completed["status"] != "running")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        ASSERT_EQ(completed["status"], "succeeded") << completed.dump();
        EXPECT_TRUE(completed["result"]["sceneSaved"].get<bool>());
        EXPECT_EQ(completed["result"]["failedAssetCount"], 1);
        ASSERT_TRUE(completed["result"]["warningsAvailable"].get<bool>());
        EXPECT_FALSE(completed["result"]["warnings"].empty());
        bool found = false;
        for (const auto& row : completed["result"]["assets"])
            found |= row["handle"] == std::to_string(static_cast<u64>(handle));
        EXPECT_TRUE(found);
        EXPECT_TRUE(fs::exists(root / output));
        bool freshScene = false;
        for (const auto& row : completed["result"]["assets"])
        {
            if (row["handle"] != std::to_string(static_cast<u64>(sceneHandle)))
                continue;
            std::ifstream packed(root / output, std::ios::binary);
            packed.seekg(row["offset"].get<std::streamoff>());
            u32 length = 0;
            packed.read(reinterpret_cast<char*>(&length), sizeof(length));
            ASSERT_LT(length, row["size"].get<u64>());
            std::string yaml(length, '\0');
            packed.read(yaml.data(), length);
            ASSERT_TRUE(packed.good());
            freshScene = yaml.find(marker) != std::string::npos;
        }
        EXPECT_TRUE(freshScene) << "The pack must contain the just-saved scene rather than the cached version.";
        auto cancelled = registry.Invoke(host, "olo_asset_pack_build", { { "operationId", operation }, { "cancel", true } }, Automation::AutomationWriteConsent::Granted);
        EXPECT_TRUE(cancelled.Result.StructuredContent["cancelRequested"].get<bool>());
    }
}
