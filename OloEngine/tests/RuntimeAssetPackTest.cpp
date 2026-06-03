#include "OloEnginePCH.h"
#include <gtest/gtest.h>

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
//      CPU-only asset type (Audio). Skips cleanly if a prior test's asset-manager
//      shutdown has cleared the shared AssetImporter serializer registry
//      (AssetImporter::Init is call_once and Shutdown clears it).
//
// All headless: scenes/audio deserialize on the CPU with no GL context.
// =============================================================================

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h> // _getpid()
inline int OloRapGetPid()
{
    return _getpid();
}
#else
#include <unistd.h>
inline int OloRapGetPid()
{
    return static_cast<int>(getpid());
}
#endif

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    // One asset entry to embed in a synthetic pack. Every entry goes into the
    // AssetInfo table; scene entries additionally get a SceneInfo whose offset
    // points at the data (the AssetInfo offset is intentionally left 0 to mirror
    // AssetPackBuilder, which never backfills scene AssetInfo offsets).
    struct PackEntry
    {
        AssetHandle Handle = 0;
        AssetType Type = AssetType::None;
        std::string Data; // raw bytes written at the entry's data offset
        bool IsScene = false;
    };

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

    // Write a valid .olopack with the given entries, computing data offsets up
    // front (no seek-back) so the layout matches what AssetPack::Load reads.
    void WritePack(const fs::path& path, const std::vector<PackEntry>& entries)
    {
        const u64 assetCount = entries.size();
        u64 sceneCount = 0;
        for (const auto& e : entries)
        {
            if (e.IsScene)
                ++sceneCount;
        }

        // Per-record byte sizes must match the field-by-field reads in AssetPack::Load.
        const u64 headerBytes = sizeof(AssetPackFile::FileHeader);
        const u64 indexBytes = sizeof(u32) * 2 + sizeof(u64) * 2;
        const u64 assetInfoBytes = sizeof(AssetHandle) + sizeof(u64) * 2 + sizeof(AssetType) + sizeof(u16);
        const u64 sceneInfoBytes = sizeof(AssetHandle) + sizeof(u64) * 2 + sizeof(u16) + sizeof(u32); // nested count = 0

        u64 cursor = headerBytes + indexBytes + assetCount * assetInfoBytes + sceneCount * sceneInfoBytes;

        std::vector<u64> dataOffset(entries.size());
        std::vector<u64> dataSize(entries.size());
        for (sizet i = 0; i < entries.size(); ++i)
        {
            dataOffset[i] = cursor;
            dataSize[i] = entries[i].Data.size();
            cursor += dataSize[i];
        }

        FileStreamWriter writer(path);
        ASSERT_TRUE(writer.IsStreamGood());

        // Header
        u32 magic = AssetPackFile::MagicNumber;
        u32 version = AssetPackFile::Version;
        u64 buildVersion = 1;
        u64 indexOffset = headerBytes;
        writer.WriteRaw(magic);
        writer.WriteRaw(version);
        writer.WriteRaw(buildVersion);
        writer.WriteRaw(indexOffset);

        // Index table
        u32 assetCount32 = static_cast<u32>(assetCount);
        u32 sceneCount32 = static_cast<u32>(sceneCount);
        u64 zero64 = 0;
        writer.WriteRaw(assetCount32);
        writer.WriteRaw(sceneCount32);
        writer.WriteRaw(zero64); // PackedAppBinaryOffset
        writer.WriteRaw(zero64); // PackedAppBinarySize

        // Asset infos: Handle, PackedOffset, PackedSize, Type, Flags
        for (sizet i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            // Scenes mirror the builder: their AssetInfo offset/size stay 0 so the
            // load path is forced through the SceneInfo table.
            u64 off = e.IsScene ? 0ull : dataOffset[i];
            u64 sz = e.IsScene ? 0ull : dataSize[i];
            u16 flags = 0;
            AssetHandle handle = e.Handle;
            AssetType type = e.Type;
            writer.WriteRaw(handle);
            writer.WriteRaw(off);
            writer.WriteRaw(sz);
            writer.WriteRaw(type);
            writer.WriteRaw(flags);
        }

        // Scene infos: Handle, PackedOffset, PackedSize, Flags, u32 sceneAssetCount
        for (sizet i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            if (!e.IsScene)
                continue;
            u16 flags = 0;
            u32 sceneAssetCount = 0;
            AssetHandle handle = e.Handle;
            u64 off = dataOffset[i];
            u64 sz = dataSize[i];
            writer.WriteRaw(handle);
            writer.WriteRaw(off);
            writer.WriteRaw(sz);
            writer.WriteRaw(flags);
            writer.WriteRaw(sceneAssetCount);
        }

        // Data section
        for (const auto& e : entries)
        {
            if (!e.Data.empty())
                writer.WriteData(e.Data.data(), e.Data.size());
        }

        ASSERT_TRUE(writer.IsStreamGood());
    }

    std::string MakeEmptySceneYaml()
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        SceneAssetSerializer serializer;
        return serializer.SerializeToString(scene);
    }
} // namespace

// -----------------------------------------------------------------------------
// 1. Scene pack deserialize round-trip (direct serializer, no manager).
// -----------------------------------------------------------------------------
TEST(RuntimeAssetPackTest, SceneSerializerPackRoundTrip)
{
    const std::string yaml = MakeEmptySceneYaml();
    ASSERT_FALSE(yaml.empty()) << "Empty scene must serialize to non-empty YAML";

    const fs::path tmp = fs::temp_directory_path() / ("olo_scene_blob_" + std::to_string(OloRapGetPid()) + ".bin");

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
    const fs::path packPath = fs::temp_directory_path() / ("olo_scene_pack_" + std::to_string(OloRapGetPid()) + ".olopack");

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
    // The shared AssetImporter serializer registry is process-global: Init() is
    // call_once and any asset manager's Shutdown() clears it. If a prior test
    // already tore an asset manager down, the registry is empty and cannot be
    // repopulated — skip rather than report a false failure.
    AssetImporter::Init();
    if (!AssetImporter::CanDeserializeFromAssetPackOffThread(AssetType::Audio))
    {
        GTEST_SKIP() << "AssetImporter serializer registry unavailable (cleared by a prior test's asset-manager shutdown)";
    }

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
    const fs::path packPath = fs::temp_directory_path() / ("olo_async_pack_" + std::to_string(OloRapGetPid()) + ".olopack");

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

    // Keep the manager alive for the process lifetime (function-local static) so its
    // destructor never clears the shared AssetImporter registry mid-suite; at process
    // exit AssetImporter::Shutdown is a no-op during static destruction. Created with
    // autoLoadDefaultPack=false.
    static Ref<RuntimeAssetManager> s_Manager = Ref<RuntimeAssetManager>::Create(false);
    RuntimeAssetManager& mgr = *s_Manager;
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
