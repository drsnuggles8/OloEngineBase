#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// =============================================================================
// SoundGraphCacheTest — persistent cache-metadata round-trip + memory accounting
//
// Pins the three behaviours that used to silently no-op / over-estimate in
// SoundGraphCache.cpp:
//
//   1. SaveCacheMetadata / LoadCacheMetadata actually serialise and restore the
//      per-entry metadata (source path, compiled path, source hash, timestamps,
//      access count) through a YAML file. Save -> Load round-trips every field.
//   2. A restored entry is metadata-only (no live graph), so it reads back as a
//      cache miss (Has == false, Get == nullptr) while its metadata is still
//      retrievable via GetCacheEntry.
//   3. Corrupt / missing metadata files fail cleanly (return false) instead of
//      throwing or corrupting cache state.
//   4. The per-entry memory estimate introspects the real WavePlayer audio-data
//      footprint instead of charging a flat 2 MB per node.
//
// CPU-only — no GL context, no audio device, no AssetManager. The graphs are
// built directly (no asset load) so the test is deterministic and headless-safe.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphCache.h"
#include "OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace OloEngine;                    // NOLINT(google-build-using-namespace)
using namespace OloEngine::Audio::SoundGraph; // NOLINT(google-build-using-namespace)

namespace
{
    // Minimal non-null graph: Put() rejects null graphs, so cache entries need a
    // real (if empty) SoundGraph instance. No nodes / no Init needed — the cache
    // never executes the graph, it only records metadata about it.
    Ref<SoundGraph> MakeEmptyGraph(const char* name)
    {
        return Ref<SoundGraph>::Create(name, UUID());
    }

    // Build a graph carrying `count` WavePlayer nodes (no audio loaded). Used to
    // exercise CalculateGraphMemoryUsage's WavePlayer introspection.
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

        // Unique-ish temp directory for this fixture's files. The test binary runs
        // from the repo root, so write to the OS temp dir to avoid polluting it.
        m_TempDir = std::filesystem::temp_directory_path() / "olo_soundgraph_cache_test";
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
        std::filesystem::create_directories(m_TempDir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    // Create a real source file (so HashFile() returns a non-zero, distinct hash
    // and GetFileModificationTime() returns a real mtime) and return its path.
    std::string MakeSourceFile(const std::string& name, const std::string& content)
    {
        const std::filesystem::path path = m_TempDir / name;
        std::ofstream fout(path);
        fout << content;
        fout.close();
        return path.string();
    }

    std::string MetadataPath() const
    {
        return (m_TempDir / "cache_metadata.yaml").string();
    }

    std::filesystem::path m_TempDir;
};

TEST_F(SoundGraphCacheTest, SaveLoadMetadataRoundTripsEveryField)
{
    // Distinct content -> distinct non-zero source hashes; real files -> real mtimes.
    const std::string srcA = MakeSourceFile("a.soundgraph", "graph A contents");
    const std::string srcB = MakeSourceFile("b.soundgraph", "graph B is different");
    const std::string srcC = MakeSourceFile("c.soundgraph", "third graph, third content");

    Ref<SoundGraphCache> saver = Ref<SoundGraphCache>::Create();
    saver->Put(srcA, MakeEmptyGraph("A"), "cache/A.sgc");
    saver->Put(srcB, MakeEmptyGraph("B"), "cache/B.sgc");
    saver->Put(srcC, MakeEmptyGraph("C"), ""); // empty compiled path is allowed

    ASSERT_TRUE(saver->SaveCacheMetadata(MetadataPath()));
    ASSERT_TRUE(std::filesystem::exists(MetadataPath()));

    Ref<SoundGraphCache> loader = Ref<SoundGraphCache>::Create();
    ASSERT_TRUE(loader->LoadCacheMetadata(MetadataPath()));

    for (const std::string& src : { srcA, srcB, srcC })
    {
        auto original = saver->GetCacheEntry(src);
        auto restored = loader->GetCacheEntry(src);
        ASSERT_TRUE(original.has_value()) << "missing saved entry for " << src;
        ASSERT_TRUE(restored.has_value()) << "missing restored entry for " << src;

        // Every persisted field must survive the round-trip exactly. Timestamps are
        // stored as raw system_clock ticks, so equality is exact integer comparison.
        EXPECT_EQ(restored->m_SourcePath, original->m_SourcePath);
        EXPECT_EQ(restored->m_CompiledPath, original->m_CompiledPath);
        EXPECT_EQ(restored->m_SourceHash, original->m_SourceHash);
        EXPECT_EQ(restored->m_LastModified, original->m_LastModified);
        EXPECT_EQ(restored->m_LastAccessed, original->m_LastAccessed);
        EXPECT_EQ(restored->m_AccessCount, original->m_AccessCount);
    }

    // Source hashes are genuinely distinct (proves the field isn't a constant 0).
    EXPECT_NE(loader->GetCacheEntry(srcA)->m_SourceHash, loader->GetCacheEntry(srcB)->m_SourceHash);
    EXPECT_NE(loader->GetCacheEntry(srcB)->m_SourceHash, loader->GetCacheEntry(srcC)->m_SourceHash);
}

TEST_F(SoundGraphCacheTest, RestoredEntryIsMetadataOnlyAndReadsAsMiss)
{
    const std::string src = MakeSourceFile("only.soundgraph", "metadata only");

    Ref<SoundGraphCache> saver = Ref<SoundGraphCache>::Create();
    saver->Put(src, MakeEmptyGraph("Only"), "cache/only.sgc");
    ASSERT_TRUE(saver->SaveCacheMetadata(MetadataPath()));

    Ref<SoundGraphCache> loader = Ref<SoundGraphCache>::Create();
    ASSERT_TRUE(loader->LoadCacheMetadata(MetadataPath()));

    // Metadata is present...
    auto entry = loader->GetCacheEntry(src);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->m_CompiledPath, "cache/only.sgc");

    // ...but the compiled graph is runtime-only and was not restored, so the cache
    // must report a miss rather than hand back a null graph on a false "hit".
    EXPECT_FALSE(loader->Has(src));
    EXPECT_EQ(loader->Get(src), nullptr);

    // GetCachedPaths only lists resident graphs, so the placeholder is excluded.
    EXPECT_TRUE(loader->GetCachedPaths().empty());
}

TEST_F(SoundGraphCacheTest, LoadMissingFileReturnsFalse)
{
    Ref<SoundGraphCache> cache = Ref<SoundGraphCache>::Create();
    const std::string missing = (m_TempDir / "does_not_exist.yaml").string();

    EXPECT_FALSE(cache->LoadCacheMetadata(missing));
    EXPECT_EQ(cache->GetSize(), 0u);
}

TEST_F(SoundGraphCacheTest, LoadCorruptFileReturnsFalseAndLeavesCacheUntouched)
{
    const std::string corrupt = (m_TempDir / "corrupt.yaml").string();
    {
        std::ofstream fout(corrupt);
        fout << "{ this is : not valid : yaml ][ : :\n\t- broken\n";
    }

    Ref<SoundGraphCache> cache = Ref<SoundGraphCache>::Create();
    EXPECT_FALSE(cache->LoadCacheMetadata(corrupt));
    EXPECT_EQ(cache->GetSize(), 0u);

    // A well-formed YAML file that simply isn't a cache-metadata document also fails.
    const std::string wrongRoot = (m_TempDir / "wrong_root.yaml").string();
    {
        std::ofstream fout(wrongRoot);
        fout << "SomethingElse:\n  Foo: 1\n";
    }
    EXPECT_FALSE(cache->LoadCacheMetadata(wrongRoot));
    EXPECT_EQ(cache->GetSize(), 0u);
}

TEST_F(SoundGraphCacheTest, LoadDoesNotClobberLiveGraph)
{
    const std::string src = MakeSourceFile("live.soundgraph", "live graph");

    // Save metadata for `src`, then in a fresh cache Put a *live* graph for the same
    // path and load the metadata over it. The live graph must survive (not be
    // downgraded to a metadata-only placeholder).
    Ref<SoundGraphCache> saver = Ref<SoundGraphCache>::Create();
    saver->Put(src, MakeEmptyGraph("Live"), "cache/live.sgc");
    ASSERT_TRUE(saver->SaveCacheMetadata(MetadataPath()));

    Ref<SoundGraphCache> cache = Ref<SoundGraphCache>::Create();
    Ref<SoundGraph> liveGraph = MakeEmptyGraph("LiveResident");
    cache->Put(src, liveGraph, "cache/live.sgc");

    ASSERT_TRUE(cache->LoadCacheMetadata(MetadataPath()));

    EXPECT_TRUE(cache->Has(src));
    EXPECT_EQ(cache->Get(src), liveGraph);
}

TEST_F(SoundGraphCacheTest, FreshWavePlayerReportsZeroAudioBytes)
{
    // The accessor the cache introspects: with no asset loaded the sample buffer is
    // empty, so the reported footprint is zero (not a flat multi-MB guess).
    WavePlayer wp("wp", UUID());
    EXPECT_EQ(wp.GetAudioDataSizeBytes(), 0u);
}

TEST_F(SoundGraphCacheTest, MemoryEstimateIntrospectsWavePlayerInsteadOfFlatPerNode)
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
