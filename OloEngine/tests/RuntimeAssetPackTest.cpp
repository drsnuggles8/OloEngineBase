#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"
#include "TestAssetPackWriter.h"

// =============================================================================
// RuntimeAssetPackTest — runtime asset-pack loading pipeline.
//
// Pins the pieces wired up to complete runtime pack loading:
//
//   1. SceneSerializerPackRoundTrip — SceneAssetSerializer::DeserializeSceneFromAssetPack
//      (previously a stub returning nullptr) round-trips a scene through the
//      on-pack scene format ([u32 size][yaml]).
//   2. SceneRoutingUsesSceneInfoOffset — scenes live in the dedicated SceneInfo
//      table; their AssetInfo record carries no valid offset (the builder leaves
//      it 0). Loading a scene MUST use SceneInfo. This pins the offset-0 bug:
//      the AssetInfo path would seek to the file header and read garbage.
//   3. OffThreadCapabilityContract — the per-serializer off-thread-safe flag that
//      gates which types the runtime async system may deserialize on a worker
//      thread. GPU-touching / asset-resolving types must report false.
//   4. AsyncLoadIntegratesThroughManager — the full async path
//      (GetAssetAsync -> worker -> SyncWithAssetThread -> loaded cache) for a
//      CPU-only asset type (Audio).
//   5. DeserializeFromAssetPackAnalyzesRealAudioMetadata — the runtime pack path
//      (AudioFileSourceSerializer::DeserializeFromAssetPack) analyzes the actual
//      audio file (via the miniaudio-backed AudioLoader) instead of returning a
//      default-constructed AudioFile (issue #598). Uses a real fixture under
//      SandboxProject/Assets, resolved through an absolute packed path so the
//      test doesn't need an active Project.
//   6. DeserializeFromAssetPackFallsBackToDefaultsWhenSourceMissing — a packed
//      path that doesn't resolve to a file on disk must not crash; it degrades to
//      the same default metadata the pre-#598 code always returned.
//
// All headless: scenes/audio deserialize on the CPU with no GL context.
// =============================================================================

#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;
    using OloEngine::Tests::PackEntry;
    using OloEngine::Tests::WritePack;

    // Serialize a scene's bytes exactly as SceneAssetSerializer::SerializeToAssetPack
    // writes them: [u32 size][char[size] yaml].
    std::string MakeSceneBlob(const std::string& yaml)
    {
        std::string blob;
        const u32 size = static_cast<u32>(yaml.size());
        blob.append(reinterpret_cast<const char*>(&size), sizeof(size));
        blob.append(yaml);
        return blob;
    }

    std::string MakeEmptySceneYaml()
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        SceneAssetSerializer serializer;
        return serializer.SerializeToString(scene);
    }
} // namespace

TEST(RuntimeAssetPackTest, SceneRegistryPathsLoadAndSaveUnderTheProjectRoot)
{
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
    const auto root = OloEngine::Tests::TempDir("scene-registry-path");
    fs::create_directories(root / "Assets/Scenes");
    ProjectConfig config;
    config.AssetDirectory = "Assets";
    ASSERT_TRUE(Project::NewInMemory(root, config));
    const auto relative = fs::path("Assets/Scenes") / ("Scene-" + root.filename().string() + ".olo");
    const auto wrongPath = fs::current_path() / relative;
    ASSERT_FALSE(fs::exists(wrongPath));
    AssetMetadata metadata;
    metadata.Handle = AssetHandle(0x1524);
    metadata.Type = AssetType::Scene;
    metadata.FilePath = relative;
    auto original = Ref<Scene>::Create();
    original->SetName("project-relative scene");
    SceneAssetSerializer serializer;
    serializer.Serialize(metadata, original);
    ASSERT_TRUE(fs::exists(root / relative));
    EXPECT_FALSE(fs::exists(wrongPath));
    Ref<Asset> loaded;
    ASSERT_TRUE(serializer.TryLoadData(metadata, loaded));
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->GetHandle(), metadata.Handle);
    EXPECT_EQ(loaded.As<Scene>()->GetName(), relative.filename().string());
    metadata.FilePath = root / relative;
    ASSERT_TRUE(serializer.TryLoadData(metadata, loaded)) << "absolute paths must remain valid";
}

// -----------------------------------------------------------------------------
// 1. Scene pack deserialize round-trip (direct serializer, no manager).
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, SceneSerializerPackRoundTrip)
{
    const std::string yaml = MakeEmptySceneYaml();
    ASSERT_FALSE(yaml.empty()) << "Empty scene must serialize to non-empty YAML";

    const fs::path tmp = OloEngine::Tests::TempFile("scene_blob.bin");

    // Scene bytes at offset 0, in the on-pack format.
    {
        FileStreamWriter writer(tmp);
        ASSERT_TRUE(writer.IsStreamGood());
        const std::string blob = MakeSceneBlob(yaml);
        writer.WriteData(blob.data(), blob.size());
    }

    AssetPackFile::SceneInfo sceneInfo;
    sceneInfo.Handle = static_cast<AssetHandle>(0x5CE9E0000ULL);
    sceneInfo.PackedOffset = 0;
    sceneInfo.PackedSize = 0; // unused by the reader

    FileStreamReader reader(tmp);
    ASSERT_TRUE(reader.IsStreamGood());

    SceneAssetSerializer serializer;
    Ref<Scene> result = serializer.DeserializeSceneFromAssetPack(reader, sceneInfo);

    ASSERT_TRUE(result) << "DeserializeSceneFromAssetPack must return a scene (was a stub returning nullptr)";
    EXPECT_EQ(result->GetHandle(), sceneInfo.Handle) << "Handle from SceneInfo must be applied";

    std::error_code ec;
    fs::remove(tmp, ec);
}

// -----------------------------------------------------------------------------
// 2. Scene routing uses the SceneInfo offset, not the (unset) AssetInfo offset.
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, SceneRoutingUsesSceneInfoOffset)
{
    const std::string yaml = MakeEmptySceneYaml();
    ASSERT_FALSE(yaml.empty());

    const AssetHandle sceneHandle = static_cast<AssetHandle>(0xABC123ULL);
    const fs::path packPath = OloEngine::Tests::TempFile("scene.olopack");

    PackEntry scene;
    scene.Handle = sceneHandle;
    scene.Type = AssetType::Scene;
    scene.Data = MakeSceneBlob(yaml);
    scene.IsScene = true;
    WritePack(packPath, { scene });

    auto pack = Ref<AssetPack>::Create();
    auto result = pack->Load(packPath);
    ASSERT_TRUE(result.Success) << "Error: " << result.ErrorMessage;

    // The bug: the scene's AssetInfo offset is 0 (points at the file header), while
    // the SceneInfo offset points at the real scene bytes.
    auto assetInfo = pack->GetAssetInfo(sceneHandle);
    ASSERT_TRUE(assetInfo.has_value()) << "Scene must also appear in the AssetInfo table";
    EXPECT_EQ(assetInfo->PackedOffset, 0u) << "Builder leaves scene AssetInfo offset unset (0)";

    auto sceneInfo = pack->GetSceneInfo(sceneHandle);
    ASSERT_TRUE(sceneInfo.has_value()) << "GetSceneInfo must resolve the scene";
    EXPECT_GT(sceneInfo->PackedOffset, 0u) << "SceneInfo offset must point past the header at the real bytes";

    // Deserializing via the SceneInfo path succeeds...
    {
        FileStreamReader reader(packPath);
        ASSERT_TRUE(reader.IsStreamGood());
        SceneAssetSerializer serializer;
        Ref<Scene> viaScene = serializer.DeserializeSceneFromAssetPack(reader, sceneInfo.value());
        EXPECT_TRUE(viaScene) << "Scene path must load the scene";
    }

    // ...while the AssetInfo path (offset 0 = file header) does not produce a valid scene.
    {
        FileStreamReader reader(packPath);
        ASSERT_TRUE(reader.IsStreamGood());
        SceneAssetSerializer serializer;
        Ref<Asset> viaAsset = serializer.DeserializeFromAssetPack(reader, assetInfo.value());
        EXPECT_FALSE(viaAsset) << "AssetInfo path (offset 0) must NOT yield a valid scene — this is the bug the routing avoids";
    }

    std::error_code ec;
    fs::remove(packPath, ec);
}

// -----------------------------------------------------------------------------
// 3. Off-thread-safe capability contract (direct serializers, no AssetImporter).
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, OffThreadCapabilityContract)
{
    // CPU-only serializers may run on a worker thread.
    EXPECT_TRUE(AudioFileSourceSerializer().CanDeserializeFromAssetPackOffThread());
    EXPECT_TRUE(MeshColliderSerializer().CanDeserializeFromAssetPackOffThread());
    EXPECT_TRUE(ScriptFileSerializer().CanDeserializeFromAssetPackOffThread());

    // Types whose deserialize resolves referenced assets / creates GPU resources
    // must stay on the main thread.
    EXPECT_FALSE(SceneAssetSerializer().CanDeserializeFromAssetPackOffThread());
    EXPECT_FALSE(PrefabSerializer().CanDeserializeFromAssetPackOffThread());
    EXPECT_FALSE(TextureSerializer().CanDeserializeFromAssetPackOffThread());

    // Base default is the conservative (safe) choice.
    EXPECT_FALSE(MaterialAssetSerializer().CanDeserializeFromAssetPackOffThread());
}

// -----------------------------------------------------------------------------
// 4. Full async path through RuntimeAssetManager for a CPU-only type (Audio).
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, AsyncLoadIntegratesThroughManager)
{
    // Bring up the engine task scheduler once per process; worker tasks never run
    // without started workers. Application does this at startup; the test binary
    // does not construct an Application.
    static const bool s_SchedulerStarted = []
    {
        LowLevelTasks::InitGameThreadId();
        Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
        LowLevelTasks::FScheduler::Get().StartWorkers();
        return true;
    }();
    (void)s_SchedulerStarted;

    const AssetHandle audioHandle = static_cast<AssetHandle>(0xA0D10ULL);
    const fs::path packPath = OloEngine::Tests::TempFile("async.olopack");

    // AudioFileSourceSerializer::DeserializeFromAssetPack reads a single string
    // (the source path) via StreamReader::ReadString — [u64 length][bytes].
    PackEntry audio;
    audio.Handle = audioHandle;
    audio.Type = AssetType::Audio;
    {
        const std::string srcPath = "sounds/test.wav";
        const u64 len = static_cast<u64>(srcPath.size());
        audio.Data.append(reinterpret_cast<const char*>(&len), sizeof(len));
        audio.Data.append(srcPath);
    }
    WritePack(packPath, { audio });

    // A plain stack-local manager: its ctor calls AssetImporter::Init(), which
    // repopulates the serializer registry if a prior test's manager shutdown cleared
    // it, so no static/leak/skip workaround is needed.
    RuntimeAssetManager mgr(/*autoLoadDefaultPack=*/false);
    ASSERT_TRUE(mgr.LoadAssetPack(packPath));

    // First async request queues the load and reports not-ready.
    AsyncAssetResult<Asset> first = mgr.GetAssetAsync(audioHandle);
    EXPECT_FALSE(first.IsReady) << "Audio is off-thread-safe, so the first request must queue (not load inline)";
    EXPECT_FALSE(first.Ptr);

    // Pump the main-thread sync until the worker delivers the asset (bounded).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool loaded = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        mgr.SyncWithAssetThread();
        if (mgr.IsAssetLoaded(audioHandle))
        {
            loaded = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(loaded) << "Async-loaded audio asset must be integrated by SyncWithAssetThread";
    if (loaded)
    {
        Ref<Asset> asset = mgr.GetAsset(audioHandle);
        ASSERT_TRUE(asset);
        EXPECT_EQ(asset->GetAssetType(), AssetType::Audio);

        // A subsequent async request is served immediately from the loaded cache.
        AsyncAssetResult<Asset> second = mgr.GetAssetAsync(audioHandle);
        EXPECT_TRUE(second.IsReady);
        EXPECT_TRUE(second.Ptr);
    }

    std::error_code ec;
    fs::remove(packPath, ec);
}

// -----------------------------------------------------------------------------
// 5. DeserializeFromAssetPack analyzes the real audio file instead of returning
//    a default-constructed AudioFile (issue #598).
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, DeserializeFromAssetPackAnalyzesRealAudioMetadata)
{
    // Real fixture, known via its RIFF fmt chunk: PCM, 2 channels, 44100 Hz,
    // 24-bit, ~979868 bytes of data -> ~3.7024s duration.
    const fs::path wavPath = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets" / "Audio" / "ding.wav";
    ASSERT_TRUE(fs::exists(wavPath)) << "Fixture missing: " << wavPath.string();
    const u64 expectedFileSize = static_cast<u64>(fs::file_size(wavPath));

    // Write the (absolute) fixture path into a synthetic pack blob exactly as
    // AudioFileSourceSerializer::SerializeToAssetPack does: a single length-prefixed
    // string. An absolute path resolves without needing an active Project.
    const fs::path tmp = OloEngine::Tests::TempFile("audio_meta.bin");
    {
        FileStreamWriter writer(tmp);
        ASSERT_TRUE(writer.IsStreamGood());
        writer.WriteString(wavPath.string());
    }

    AssetPackFile::AssetInfo assetInfo;
    assetInfo.Handle = static_cast<AssetHandle>(0xA0D10ULL);
    assetInfo.PackedOffset = 0;
    assetInfo.PackedSize = 0; // unused by the reader
    assetInfo.Type = AssetType::Audio;
    assetInfo.Flags = 0;

    FileStreamReader reader(tmp);
    ASSERT_TRUE(reader.IsStreamGood());

    AudioFileSourceSerializer serializer;
    Ref<Asset> result = serializer.DeserializeFromAssetPack(reader, assetInfo);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->GetHandle(), assetInfo.Handle);

    Ref<AudioFile> audioFile = result.As<AudioFile>();
    ASSERT_TRUE(audioFile);
    EXPECT_EQ(audioFile->GetSamplingRate(), 44100u);
    EXPECT_EQ(audioFile->GetBitDepth(), 24u);
    EXPECT_EQ(audioFile->GetNumChannels(), 2u);
    EXPECT_EQ(audioFile->GetFileSize(), expectedFileSize);
    EXPECT_NEAR(audioFile->GetDuration(), 3.7024, 0.01)
        << "Duration must reflect the real decoded frame count, not the pre-#598 default of 0";

    std::error_code ec;
    fs::remove(tmp, ec);
}

// -----------------------------------------------------------------------------
// 6. A packed path that can't be resolved/opened must not crash - it degrades to
//    the same default metadata the code always returned before #598.
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, DeserializeFromAssetPackFallsBackToDefaultsWhenSourceMissing)
{
    const fs::path missingPath = OloEngine::Tests::TempFile("missing_audio.wav");
    std::error_code removeEc;
    fs::remove(missingPath, removeEc); // make sure it really doesn't exist

    const fs::path tmp = OloEngine::Tests::TempFile("audio_meta_missing.bin");
    {
        FileStreamWriter writer(tmp);
        ASSERT_TRUE(writer.IsStreamGood());
        writer.WriteString(missingPath.string());
    }

    AssetPackFile::AssetInfo assetInfo;
    assetInfo.Handle = static_cast<AssetHandle>(0xA0D11ULL);
    assetInfo.PackedOffset = 0;
    assetInfo.PackedSize = 0;
    assetInfo.Type = AssetType::Audio;
    assetInfo.Flags = 0;

    FileStreamReader reader(tmp);
    ASSERT_TRUE(reader.IsStreamGood());

    AudioFileSourceSerializer serializer;
    Ref<Asset> result = serializer.DeserializeFromAssetPack(reader, assetInfo);
    ASSERT_TRUE(result) << "A missing source file must still yield a default-metadata AudioFile, not nullptr";

    Ref<AudioFile> audioFile = result.As<AudioFile>();
    ASSERT_TRUE(audioFile);
    EXPECT_EQ(audioFile->GetSamplingRate(), 44100u);
    EXPECT_EQ(audioFile->GetBitDepth(), 16u);
    EXPECT_EQ(audioFile->GetNumChannels(), 2u);
    EXPECT_NEAR(audioFile->GetDuration(), 0.0, 1e-6);
    EXPECT_EQ(audioFile->GetFileSize(), 0u);

    std::error_code ec;
    fs::remove(tmp, ec);
}
