#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"
#include "Rendering/PropertyTests/ScopedWarningCapture.h"

// OLO_TEST_LAYER: unit
//
// =============================================================================
// SoundGraphCacheTest — per-node memory accounting
//
// SoundGraphCache is a purely in-memory LRU cache of live compiled graphs; its
// cross-run persistence is owned by CompilerCache (SaveToDisk / LoadFromDisk),
// not by this class, so there is nothing on-disk to round-trip here.
//
// What this pins is the memory estimate: CalculateGraphMemoryUsage must measure
// the real per-node heap footprint via the NodeProcessor::GetHeapBytes() hook
// (a WavePlayer reports its decoded-sample buffer; everything else reports 0),
// instead of charging a flat multi-MB-per-node guess.
//
// CPU-only — no GL context, no audio device, no AssetManager. Graphs are built
// directly (no asset load) so the test is deterministic and headless-safe.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Audio/SoundGraph/CompilerCache.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphCache.h"
#include "OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h"

#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace OloEngine;                    // NOLINT(google-build-using-namespace)
using namespace OloEngine::Audio::SoundGraph; // NOLINT(google-build-using-namespace)

namespace
{
    // Build a graph carrying `count` WavePlayer nodes (no audio loaded). Used to
    // exercise CalculateGraphMemoryUsage's GetHeapBytes() introspection.
    Ref<SoundGraph> MakeGraphWithWavePlayers(sizet count)
    {
        Ref<SoundGraph> graph = Ref<SoundGraph>::Create("WavePlayers", UUID());
        for (sizet i = 0; i < count; ++i)
            graph->AddNode(CreateScope<WavePlayer>("wp", UUID()));
        return graph;
    }
} // namespace

class SoundGraphCacheTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Initialize();

        m_TempDir = OloEngine::Tests::TempDir();
    }

    // No TearDown: TempDir() empties this directory at the start of every test,
    // and the process root is swept at exit — so wiping here would be dead work
    // that also defeats --olo-keep-temp.

    // Create a real source file so Put()'s HashFile()/GetFileModificationTime()
    // operate on an existing path, and return that path.
    std::string MakeSourceFile(const std::string& name, const std::string& content) const
    {
        const std::filesystem::path path = m_TempDir / name;
        std::ofstream fout(path);
        fout << content;
        fout.close();
        return path.string();
    }

    std::filesystem::path m_TempDir;
};

TEST_F(SoundGraphCacheTest, FreshWavePlayerReportsZeroHeapBytes)
{
    // With no asset loaded the sample buffer is empty, so the reported footprint is
    // zero (not a flat multi-MB guess). The GetHeapBytes() hook the cache consumes
    // must agree with the concrete accessor.
    WavePlayer wp("wp", UUID());
    EXPECT_EQ(wp.GetAudioDataSizeBytes(), 0u);
    EXPECT_EQ(wp.GetHeapBytes(), 0u);
    EXPECT_EQ(wp.GetHeapBytes(), wp.GetAudioDataSizeBytes());
}

TEST_F(SoundGraphCacheTest, MemoryEstimateIntrospectsNodeHeapInsteadOfFlatPerNode)
{
    // Old behaviour charged a hardcoded 2 MB per node. A 3-WavePlayer graph with no
    // audio loaded would have been estimated at >= 6 MB; with real introspection the
    // empty sample buffers contribute nothing, so the estimate stays well under 1 MB.
    constexpr sizet kNodeCount = 3;
    const std::string src = MakeSourceFile("waveplayers.soundgraph", "wave players");

    Ref<SoundGraphCache> cache = Ref<SoundGraphCache>::Create();
    cache->Put(src, MakeGraphWithWavePlayers(kNodeCount), "cache/wp.sgc");

    const sizet reported = cache->GetMemoryUsage();
    const sizet oldFlatEstimate = kNodeCount * 2u * 1024u * 1024u; // the retired 2 MB/node floor

    EXPECT_GT(reported, 0u);
    EXPECT_LT(reported, 1024u * 1024u) << "estimate should reflect (empty) audio buffers, not a flat per-node guess";
    EXPECT_LT(reported, oldFlatEstimate) << "estimate must be far below the retired 2 MB/node formula";
}

// CompilerCache's ".compiled" files carry "OLCC" + CompilerCache::FormatVersion. The reader
// accepts exactly that version: a file of any other version is not loaded (a cache miss, so the
// graph is recompiled), never read with a guessed layout.
TEST_F(SoundGraphCacheTest, CompilerCacheRejectsAnotherFormatVersion)
{
    const std::string cacheDir = (m_TempDir / "compiler").string();
    const std::string source = MakeSourceFile("graph.sgraph", "graph");

    CompilationResult result;
    result.m_SourcePath = source;
    result.m_CompilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION;
    result.m_CompiledData = { 1, 2, 3, 4 };
    result.m_IsValid = true;
    result.m_CompilationTime = std::chrono::system_clock::now();

    std::string cacheFile;
    {
        CompilerCache writer(cacheDir);
        writer.SetAutoSave(false);
        writer.StoreCompiled(source, result);
        ASSERT_TRUE(writer.SaveToDisk());
        cacheFile = writer.GetCacheFilePath(source);
    }
    ASSERT_TRUE(std::filesystem::exists(cacheFile));

    {
        CompilerCache reader(cacheDir);
        reader.SetAutoSave(false);
        ASSERT_EQ(reader.GetCacheSize(), 1u) << "positive control: the current version loads";
    }

    for (const u32 otherVersion : { CompilerCache::FormatVersion - 1, CompilerCache::FormatVersion + 1 })
    {
        {
            std::fstream file(cacheFile, std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(file.is_open());
            file.seekp(4); // after the "OLCC" magic; the version is a little-endian u32
            const u8 bytes[4] = { static_cast<u8>(otherVersion), static_cast<u8>(otherVersion >> 8),
                                  static_cast<u8>(otherVersion >> 16), static_cast<u8>(otherVersion >> 24) };
            file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        }
        CompilerCache reader(cacheDir);
        reader.SetAutoSave(false);
        EXPECT_EQ(reader.GetCacheSize(), 0u) << "version " << otherVersion;
        EXPECT_EQ(reader.GetCompiled(source).get(), nullptr);
    }
}

// Every length in a ".compiled" file is bounded by the bytes left in the file. A string length
// patched to 0xFFFFFFF0 used to reach std::string(length) directly: an attempted ~4 GiB
// allocation that either threw or succeeded and then read short. It is now a cache miss that
// names the overrun before anything is allocated.
TEST_F(SoundGraphCacheTest, CompilerCacheRejectsALengthLongerThanTheFile)
{
    const std::string cacheDir = (m_TempDir / "compiler").string();
    const std::string source = MakeSourceFile("graph.sgraph", "graph");

    CompilationResult result;
    result.m_SourcePath = source;
    result.m_CompilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION;
    result.m_CompiledData = { 1, 2, 3, 4 };
    result.m_IsValid = true;
    result.m_CompilationTime = std::chrono::system_clock::now();

    std::string cacheFile;
    {
        CompilerCache writer(cacheDir);
        writer.SetAutoSave(false);
        writer.StoreCompiled(source, result);
        ASSERT_TRUE(writer.SaveToDisk());
        cacheFile = writer.GetCacheFilePath(source);
    }
    {
        CompilerCache reader(cacheDir);
        reader.SetAutoSave(false);
        ASSERT_EQ(reader.GetCacheSize(), 1u) << "positive control: the intact file loads";
    }

    {
        std::fstream file(cacheFile, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(8); // "OLCC" + u32 version, then the source path's little-endian u32 length
        constexpr u32 kHugeLength = 0xFFFFFFF0u;
        const u8 bytes[4] = { static_cast<u8>(kHugeLength), static_cast<u8>(kHugeLength >> 8),
                              static_cast<u8>(kHugeLength >> 16), static_cast<u8>(kHugeLength >> 24) };
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    OloEngine::Tests::ScopedWarningCapture capture;
    CompilerCache reader(cacheDir);
    reader.SetAutoSave(false);
    EXPECT_EQ(reader.GetCacheSize(), 0u);
    EXPECT_EQ(reader.GetCompiled(source).get(), nullptr);
    EXPECT_EQ(capture.Count("declares a string longer than the file"), 1u)
        << "the entry must be refused by the length bound, not by a failed allocation or a short read";
}

TEST_F(SoundGraphCacheTest, CompilerCacheDirectorySwitchSavesThePreviousCacheWithoutDeadlocking)
{
    const std::string originalDirectory = (m_TempDir / "original").string();
    const std::string nextDirectory = (m_TempDir / "next").string();
    const std::string source = MakeSourceFile("graph.sgraph", "graph");

    CompilationResult result;
    result.m_SourcePath = source;
    result.m_CompilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION;
    result.m_CompiledData = { 1, 2, 3, 4 };
    result.m_IsValid = true;
    result.m_CompilationTime = std::chrono::system_clock::now();

    auto cache = std::make_shared<CompilerCache>(originalDirectory);
    cache->SetAutoSave(false);
    cache->StoreCompiled(source, result);
    cache->SetAutoSave(true);

    // Keep all worker state alive if a regression parks inside the non-recursive
    // cache mutex. A detached thread lets the failing process exit after the
    // liveness budget instead of hanging while a future/thread destructor joins.
    auto switched = std::make_shared<std::promise<void>>();
    auto completed = switched->get_future();
    std::thread worker([cache, nextDirectory, switched]
                       {
                           cache->SetCacheDirectory(nextDirectory);
                           switched->set_value(); });
    worker.detach();
    ASSERT_EQ(completed.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "SetCacheDirectory must not call a self-locking SaveToDisk while holding m_Mutex";
    completed.get();

    EXPECT_EQ(cache->GetCacheDirectory(), nextDirectory);
    EXPECT_EQ(cache->GetCacheSize(), 0u);

    // Returning without saving would avoid the deadlock but lose the old cache.
    CompilerCache original(originalDirectory);
    original.SetAutoSave(false);
    ASSERT_EQ(original.GetCacheSize(), 1u);
    const auto restored = original.GetCompiled(source);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->m_CompiledData, result.m_CompiledData);
}

TEST_F(SoundGraphCacheTest, CompilerCacheDirectorySwitchResetsEvictionState)
{
    CompilerCache cache((m_TempDir / "original").string());
    cache.SetAutoSave(false);
    cache.SetMaxCacheSize(1);

    const std::string oldSource = MakeSourceFile("old.sgraph", "old");
    const std::string firstSource = MakeSourceFile("first.sgraph", "first");
    const std::string secondSource = MakeSourceFile("second.sgraph", "second");
    CompilationResult result;
    result.m_CompilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION;
    result.m_IsValid = true;
    result.m_CompilationTime = std::chrono::system_clock::now();

    result.m_SourcePath = oldSource;
    cache.StoreCompiled(oldSource, result);
    cache.SetCacheDirectory((m_TempDir / "next").string());
    ASSERT_EQ(cache.GetCacheSize(), 0u);

    result.m_SourcePath = firstSource;
    cache.StoreCompiled(firstSource, result);
    result.m_SourcePath = secondSource;
    cache.StoreCompiled(secondSource, result);

    EXPECT_EQ(cache.GetCacheSize(), 1u) << "old-directory LRU keys must not consume an eviction";
    EXPECT_EQ(cache.GetCompiled(firstSource), nullptr);
    EXPECT_NE(cache.GetCompiled(secondSource), nullptr);
}

TEST_F(SoundGraphCacheTest, CompilerCacheReloadDoesNotDuplicateEvictionState)
{
    CompilerCache cache((m_TempDir / "compiler").string());
    cache.SetAutoSave(false);
    cache.SetMaxCacheSize(2);

    CompilationResult result;
    result.m_CompilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION;
    result.m_IsValid = true;
    for (const char* name : { "first.sgraph", "second.sgraph" })
    {
        result.m_SourcePath = MakeSourceFile(name, "graph");
        result.m_CompilationTime = std::chrono::system_clock::now();
        cache.StoreCompiled(result.m_SourcePath, result);
    }
    ASSERT_TRUE(cache.SaveToDisk());
    ASSERT_TRUE(cache.LoadFromDisk());
    ASSERT_EQ(cache.GetCacheSize(), 2u);

    // Three evictions traverse beyond the original two keys. Duplicate LRU
    // nodes left by a reload would evict an absent key and exceed the limit.
    for (const char* name : { "third.sgraph", "fourth.sgraph", "fifth.sgraph" })
    {
        result.m_SourcePath = MakeSourceFile(name, "graph");
        result.m_CompilationTime = std::chrono::system_clock::now();
        cache.StoreCompiled(result.m_SourcePath, result);
        EXPECT_EQ(cache.GetCacheSize(), 2u) << name;
    }
    EXPECT_NE(cache.GetCompiled((m_TempDir / "fourth.sgraph").string()), nullptr);
    EXPECT_NE(cache.GetCompiled((m_TempDir / "fifth.sgraph").string()), nullptr);
}

TEST_F(SoundGraphCacheTest, CompilerCacheReportsDiskInitializationFailures)
{
    const std::string regularFile = MakeSourceFile("blocker", "not a directory");

    // Directory creation reports failure internally when a parent is a file.
    // The constructor must retain that return value instead of claiming success.
    CompilerCache blocked((std::filesystem::path(regularFile) / "compiler").string());
    blocked.SetAutoSave(false);
    EXPECT_FALSE(blocked.IsDirectoryInitialized());
    EXPECT_FALSE(blocked.IsFullyInitialized());
    EXPECT_FALSE(blocked.GetInitializationErrors().empty());

    // Loading an existing regular file as a directory fails inside LoadFromDisk.
    CompilerCache unreadable(regularFile);
    unreadable.SetAutoSave(false);
    EXPECT_FALSE(unreadable.IsDirectoryInitialized());
    EXPECT_FALSE(unreadable.IsDiskCacheLoaded());
    EXPECT_FALSE(unreadable.IsFullyInitialized());
    EXPECT_FALSE(unreadable.GetInitializationErrors().empty());
}

TEST_F(SoundGraphCacheTest, CompilerCacheMaintenanceRemovalsResetEvictionState)
{
    for (const bool cleanupExpired : { false, true })
    {
        const std::string label = cleanupExpired ? "expired" : "missing";
        SCOPED_TRACE(label);
        CompilerCache cache((m_TempDir / label).string());
        cache.SetAutoSave(false);
        cache.SetMaxCacheSize(1);

        CompilationResult result;
        result.m_SourcePath = MakeSourceFile(label + ".sgraph", "graph");
        result.m_CompilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION;
        result.m_IsValid = true;
        result.m_CompilationTime = std::chrono::system_clock::now() - std::chrono::hours(48);
        cache.StoreCompiled(result.m_SourcePath, result);
        if (cleanupExpired)
        {
            cache.CleanupOldEntries(std::chrono::hours(24));
        }
        else
        {
            ASSERT_TRUE(std::filesystem::remove(result.m_SourcePath));
            cache.ValidateAllEntries();
        }
        ASSERT_EQ(cache.GetCacheSize(), 0u);

        const std::string firstSource = MakeSourceFile(label + "_first.sgraph", "first");
        const std::string secondSource = MakeSourceFile(label + "_second.sgraph", "second");
        result.m_CompilationTime = std::chrono::system_clock::now();
        result.m_SourcePath = firstSource;
        cache.StoreCompiled(firstSource, result);
        result.m_SourcePath = secondSource;
        cache.StoreCompiled(secondSource, result);

        EXPECT_EQ(cache.GetCacheSize(), 1u) << "removed entries must not consume later evictions";
        EXPECT_EQ(cache.GetCompiled(firstSource), nullptr);
        EXPECT_NE(cache.GetCompiled(secondSource), nullptr);
    }
}
