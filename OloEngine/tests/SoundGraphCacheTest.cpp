#include "OloEnginePCH.h"
#include <gtest/gtest.h>

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

        // Per-test-case subdirectory so cases running concurrently under
        // `ctest --parallel` don't share a path. gtest_discover_tests registers each
        // case as its own ctest entry (its own process), and SetUp() does a
        // remove_all — a fixed shared name would let one case wipe another's files
        // mid-run. Keyed by suite+case name per docs/agent-rules/testing-architecture.md.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string testSuite = info ? info->test_suite_name() : "SoundGraphCacheTest";
        const std::string testName = info ? info->name() : "Unknown";
        m_TempDir = std::filesystem::temp_directory_path() / "olo_soundgraph_cache_test" / (testSuite + "_" + testName);
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
        std::filesystem::create_directories(m_TempDir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_TempDir, ec);
    }

    // Create a real source file so Put()'s HashFile()/GetFileModificationTime()
    // operate on an existing path, and return that path.
    std::string MakeSourceFile(const std::string& name, const std::string& content)
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
