#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

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
#include <string>

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
