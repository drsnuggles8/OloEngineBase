// OLO_TEST_LAYER: plumbing
// =============================================================================
// VirtualGeometryPageStoreTest.cpp — the on-disk backing store beneath
// VirtualMeshRegistry::LoadPage (issue #1151).
//
// The contract this pins is narrow and entirely CPU-side: bytes written for a
// page come back byte-identical, page-LOCAL, through both the blocking and the
// asynchronous route; a request that cannot be served says Failed rather than
// handing back a short buffer; and an outstanding read is a counted Pending,
// never a silent empty page.
//
// That last one is the reason this file exists at all. A streaming path whose
// failure mode is "renders less geometry" has no natural alarm — the frame
// still draws, it is just coarser — so every "not yet" and every "never" has to
// be observable from the store's own counters, and those counters are what the
// editor panel and olo_virtual_geometry_stats report. A regression that turned
// a failed read into a zero-filled page would leave every picture plausible.
//
// No GL context is touched anywhere here: the store reads and writes files.
// =============================================================================

#include "OloEngine/Renderer/VirtualGeometry/VirtualGeometryPageStore.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace OloEngine;

namespace
{
    // A synthetic packed mesh whose every value is derived from its index, so a page read
    // back at the wrong offset (or from the wrong page) is a mismatch rather than a
    // plausible-looking sphere.
    VirtualMeshGpuData MakePackedMesh(u32 pageCount, u32 verticesPerPage, u32 indicesPerPage,
                                      bool withLightmapUVs)
    {
        VirtualMeshGpuData data;
        for (u32 page = 0; page < pageCount; ++page)
        {
            VirtualPageInfo info;
            info.GroupIndex = page;
            info.VertexOffset = page * verticesPerPage;
            info.VertexCount = verticesPerPage;
            info.IndexOffset = page * indicesPerPage;
            info.IndexCount = indicesPerPage;
            info.Pinned = (page == pageCount - 1);
            data.Pages.Add(info);

            for (u32 v = 0; v < verticesPerPage; ++v)
            {
                auto const seed = static_cast<f32>(page * 1000 + v);
                VirtualGpuVertex vertex;
                vertex.PositionU = glm::vec4(seed, seed + 1.0f, seed + 2.0f, seed + 3.0f);
                vertex.NormalV = glm::vec4(seed + 4.0f, seed + 5.0f, seed + 6.0f, seed + 7.0f);
                data.Vertices.Add(vertex);
                if (withLightmapUVs)
                {
                    data.LightmapUVs.Emplace(seed + 8.0f, seed + 9.0f);
                }
            }
            for (u32 i = 0; i < indicesPerPage; ++i)
            {
                data.Indices.Add(page * 1000 + i);
            }
        }
        // The registry only spills entries whose DAG built, and IsValid() gates that — give
        // the fixture the one cluster/group it takes to look like a real cook.
        data.Clusters.Emplace();
        data.Groups.Emplace();
        return data;
    }

    void ExpectPageMatches(const VirtualPagePayload& payload, const VirtualMeshGpuData& source, u32 page,
                           bool withLightmapUVs)
    {
        const VirtualPageInfo& info = source.Pages[page];
        ASSERT_EQ(static_cast<sizet>(payload.Vertices.Num()), info.VertexCount);
        ASSERT_EQ(static_cast<sizet>(payload.Indices.Num()), info.IndexCount);
        ASSERT_EQ(static_cast<sizet>(payload.LightmapUVs.Num()), withLightmapUVs ? info.VertexCount : 0u);

        for (u32 v = 0; v < info.VertexCount; ++v)
        {
            // Bit-exact: these are bytes off a disk, not the result of arithmetic, so an
            // epsilon here would hide exactly the offset-by-one-page bug being hunted.
            const VirtualGpuVertex& expected = source.Vertices[info.VertexOffset + v];
            const VirtualGpuVertex& actual = payload.Vertices[v];
            EXPECT_EQ(std::memcmp(&expected, &actual, sizeof(VirtualGpuVertex)), 0)
                << "page " << page << " vertex " << v << " came back from the wrong file offset";
            if (withLightmapUVs)
            {
                const glm::vec2& expectedUV = source.LightmapUVs[info.VertexOffset + v];
                const glm::vec2& actualUV = payload.LightmapUVs[v];
                EXPECT_EQ(std::memcmp(&expectedUV, &actualUV, sizeof(glm::vec2)), 0)
                    << "page " << page << " uv2 " << v << " came back from the wrong file offset";
            }
        }
        for (u32 i = 0; i < info.IndexCount; ++i)
        {
            EXPECT_EQ(payload.Indices[i], source.Indices[info.IndexOffset + i])
                << "page " << page << " index " << i << " came back from the wrong file offset";
        }
    }

    // Drives Fetch until it settles on Ready or Failed. BOTH Pending and Deferred mean "ask
    // again later" — Pending has a read outstanding, Deferred has not started one because the
    // in-flight cap is full — so both keep the loop going. The store's workers are real
    // threads doing real file IO, so the only honest wait is a bounded poll; a timeout here is
    // a genuine failure (a wedged queue), not flakiness, which is why it is generous.
    VirtualGeometryPageStore::FetchState FetchUntilSettled(VirtualGeometryPageStore& store, u32 base, u32 page,
                                                           const VirtualPagePayload*& payload)
    {
        using namespace std::chrono;
        auto const deadline = steady_clock::now() + seconds(10);
        for (;;)
        {
            auto const state = store.Fetch(base, page, payload);
            if (state != VirtualGeometryPageStore::FetchState::Pending &&
                state != VirtualGeometryPageStore::FetchState::Deferred)
            {
                return state;
            }
            if (steady_clock::now() > deadline)
            {
                return VirtualGeometryPageStore::FetchState::Pending;
            }
            std::this_thread::sleep_for(milliseconds(1));
        }
    }

    class VirtualGeometryPageStoreTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Tests::TempDir() rather than the shared temp root: every gtest case runs in its
            // own process and CI runs them in parallel, so a fixed path would be shared
            // mutable state (TestTempDir.h). It matters more than usual here — this fixture
            // asserts on which files EXIST in its directory, so a sibling's spill turning up
            // would fail the orphan-sweep case for no reason of its own.
            m_Directory = OloEngine::Tests::TempDir("vgpagestore");
            std::error_code ec;
            std::filesystem::remove_all(m_Directory, ec);
            std::filesystem::create_directories(m_Directory, ec);
            VirtualGeometryPageStore::SetSpillDirectory(m_Directory);
        }

        void TearDown() override
        {
            VirtualGeometryPageStore::SetSpillDirectory({});
            std::error_code ec;
            std::filesystem::remove_all(m_Directory, ec);
        }

        std::filesystem::path m_Directory;
    };
} // namespace

// A page written into the store reads back byte-identical, and PAGE-LOCAL: the registry
// rebases cluster records onto slot-local indices assuming the payload starts at 0, so a
// payload that carried the mesh-global window would draw a different page's geometry at
// every offset — and still draw.
TEST_F(VirtualGeometryPageStoreTest, BlockingReadRoundTripsEveryPage)
{
    constexpr u32 kPages = 6;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, 17, 33, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);
    ASSERT_TRUE(store.IsOpen());
    EXPECT_EQ(store.GetStats().PagesWritten, kPages);
    EXPECT_GT(store.GetStats().BytesWritten, 0u);

    for (u32 page = 0; page < kPages; ++page)
    {
        VirtualPagePayload payload;
        ASSERT_TRUE(store.ReadPageBlocking(base, page, payload)) << "page " << page;
        ExpectPageMatches(payload, source, page, /*withLightmapUVs=*/false);
    }
    EXPECT_EQ(store.GetStats().BlockingReads, kPages);
    EXPECT_EQ(store.GetStats().ReadsFailed, 0u);
}

// The uv2 stream rides the same page window as the vertices and must come back on the same
// page-local indexing — the registry packs it four pairs to an element with no per-page
// fixup, so a shifted uv2 window is a lightmap sampled from a neighbouring chart.
TEST_F(VirtualGeometryPageStoreTest, LightmapUVsRoundTripAlongsideVertices)
{
    constexpr u32 kPages = 4;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, 9, 12, /*withLightmapUVs=*/true);

    VirtualGeometryPageStore store;
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    for (u32 page = 0; page < kPages; ++page)
    {
        VirtualPagePayload payload;
        ASSERT_TRUE(store.ReadPageBlocking(base, page, payload)) << "page " << page;
        ExpectPageMatches(payload, source, page, /*withLightmapUVs=*/true);
    }
}

// The asynchronous route is the one the per-frame request path uses. Two properties, and
// they are the whole reason LoadPage was split into fetch-then-allocate: a page that has not
// arrived answers Pending (so the caller leaves it non-resident rather than uploading
// nothing), and the same bytes arrive as on the blocking route.
TEST_F(VirtualGeometryPageStoreTest, AsyncFetchDeliversTheSameBytesAndCountsTheWait)
{
    constexpr u32 kPages = 8;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, 21, 45, /*withLightmapUVs=*/true);

    VirtualGeometryPageStore store;
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    // The very first Fetch of a page cannot be Ready — nothing has read it yet. This is the
    // assertion that would fail if the store ever started serving a page synchronously from
    // inside Fetch, which would put a disk read on the render thread.
    const VirtualPagePayload* firstLook = nullptr;
    EXPECT_EQ(store.Fetch(base, 0, firstLook), VirtualGeometryPageStore::FetchState::Pending);
    EXPECT_EQ(firstLook, nullptr);
    EXPECT_GE(store.GetStats().ReadsIssued, 1u);

    for (u32 page = 0; page < kPages; ++page)
    {
        const VirtualPagePayload* payload = nullptr;
        ASSERT_EQ(FetchUntilSettled(store, base, page, payload), VirtualGeometryPageStore::FetchState::Ready)
            << "page " << page << " never arrived — the IO queue is wedged";
        ASSERT_NE(payload, nullptr);
        ExpectPageMatches(*payload, source, page, /*withLightmapUVs=*/true);
        store.Release(base, page);
    }

    EXPECT_EQ(store.GetStats().ReadsCompleted, kPages);
    EXPECT_EQ(store.GetStats().ReadsFailed, 0u);
    EXPECT_GT(store.GetStats().BytesRead, 0u);
    // Every payload was released, so the store is holding no staging RAM. A leak here is a
    // slow unbounded growth in the exact path that exists to BOUND memory.
    EXPECT_EQ(store.GetStats().StagingBytes, 0u);
    EXPECT_EQ(store.GetStats().PagesReady, 0u);
    EXPECT_GT(store.GetStats().PeakStagingBytes, 0u);
}

// Staging RAM is bounded by how many reads may be outstanding, and the cap must hold even
// when a frame asks for far more pages than it can absorb. Over the cap the answer is
// Pending (the page is still coming, just not asked for yet), never Failed.
TEST_F(VirtualGeometryPageStoreTest, OutstandingReadsAreCappedRatherThanUnbounded)
{
    constexpr u32 kPages = 64;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, 32, 64, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    store.SetMaxReadsInFlight(4);
    EXPECT_EQ(store.GetMaxReadsInFlight(), 4u);
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    u32 peakInFlight = 0;
    for (u32 page = 0; page < kPages; ++page)
    {
        const VirtualPagePayload* payload = nullptr;
        auto const state = store.Fetch(base, page, payload);
        EXPECT_NE(state, VirtualGeometryPageStore::FetchState::Failed)
            << "page " << page << " was refused rather than deferred — backpressure must not look like a "
                                  "read error";
        peakInFlight = std::max(peakInFlight, store.GetStats().ReadsInFlight);
    }
    // The CAP, not the total. How many reads the burst issued in total is timing-dependent —
    // the workers drain the queue while this loop runs — but the number outstanding at any
    // instant is exactly what the cap governs, so that is what to assert on.
    EXPECT_LE(peakInFlight, 4u)
        << "reads outstanding exceeded the 4-read cap — staging RAM is unbounded";

    // ...and every page still arrives eventually, one retry round at a time.
    for (u32 page = 0; page < kPages; ++page)
    {
        const VirtualPagePayload* payload = nullptr;
        ASSERT_EQ(FetchUntilSettled(store, base, page, payload), VirtualGeometryPageStore::FetchState::Ready)
            << "page " << page << " never arrived under backpressure";
        store.Release(base, page);
    }
    EXPECT_EQ(store.GetStats().ReadsFailed, 0u);
    EXPECT_EQ(store.GetStats().StagingBytes, 0u);
}

// The other half of the staging bound, and the one that is easy to miss. Capping only the
// reads IN FLIGHT bounds nothing: a page is fetched because its group was requested and
// released when a consumer uploads it, and in between the camera can move — the group stops
// being requested, nobody ever consumes the payload, and it sits in the ready set forever.
// Left unbounded, a moving camera over a large scene re-accumulates in RAM exactly the payload
// the spill existed to remove. Measured on the VirtualGeometryStress scene before this cap:
// 551 staged pages / 37 MB after a minute on a STATIC camera, still climbing.
TEST_F(VirtualGeometryPageStoreTest, StagedPayloadsNobodyConsumesAreCappedAndCounted)
{
    constexpr u32 kPages = 48;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, 24, 48, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    store.SetMaxStagedPages(4);
    EXPECT_EQ(store.GetMaxStagedPages(), 4u);
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    // Fetch every page repeatedly and deliberately Release NOTHING — the "requested, then the
    // camera moved on" case. A fixed number of rounds rather than "until everything is ready",
    // because under a working cap everything is NEVER all ready at once; that is the point.
    using namespace std::chrono;
    constexpr int kRounds = 40;
    for (int round = 0; round < kRounds; ++round)
    {
        for (u32 page = 0; page < kPages; ++page)
        {
            const VirtualPagePayload* payload = nullptr;
            (void)store.Fetch(base, page, payload);
        }
        EXPECT_LE(store.GetStats().PagesReady, 4u)
            << "the staged set exceeded its cap at round " << round
            << " — unconsumed payloads are unbounded";
        std::this_thread::sleep_for(milliseconds(2));
    }

    EXPECT_LE(store.GetStats().PagesReady, 4u);
    EXPECT_GT(store.GetStats().StagedDiscards, 0u)
        << "nothing was ever discarded, so the cap was never actually exercised — this test is "
           "asserting on a case it did not reach";
    // Bytes must track the map: a discard that forgot to subtract would leave the counter
    // claiming RAM the store no longer holds, and the cap would then read as "already full".
    EXPECT_LE(store.GetStats().StagingBytes, 4ull * 24ull * sizeof(VirtualGpuVertex) +
                                                 4ull * 48ull * sizeof(u32));
}

// A page that is released and later staged again must not confuse the oldest-first trim. The
// order queue then holds its key twice, and with only the key to go on the stale front entry
// still names a live map entry — so the trim deletes the NEWEST payload while older ones stay.
// A page the request loop reaches first each frame could then be read, trimmed and re-read
// forever without ever becoming resident, at full disk cost and with nothing in the log.
TEST_F(VirtualGeometryPageStoreTest, ReStagingAPageDoesNotMakeTheTrimEvictTheNewestPayload)
{
    constexpr u32 kPages = 12;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, 8, 12, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    store.SetMaxStagedPages(3);
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    // Stage page 0, release it, then stage it again — this is what leaves a stale duplicate
    // key in the order queue.
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        const VirtualPagePayload* payload = nullptr;
        ASSERT_EQ(FetchUntilSettled(store, base, 0, payload), VirtualGeometryPageStore::FetchState::Ready);
        store.Release(base, 0);
    }
    const VirtualPagePayload* zero = nullptr;
    ASSERT_EQ(FetchUntilSettled(store, base, 0, zero), VirtualGeometryPageStore::FetchState::Ready);
    store.Release(base, 0, /*keepStaged=*/true);

    // Now push three more pages through so the cap has to discard. Page 0 is the OLDEST live
    // staging, so it is the one that must go; the newly staged pages must survive.
    for (u32 page = 1; page <= 3; ++page)
    {
        const VirtualPagePayload* payload = nullptr;
        ASSERT_EQ(FetchUntilSettled(store, base, page, payload), VirtualGeometryPageStore::FetchState::Ready)
            << "page " << page << " never arrived";
        // keepStaged: the payload stays in the staged set (so the cap has something to act on)
        // but its lease ends, which is what makes it a legal eviction candidate.
        store.Release(base, page, /*keepStaged=*/true);
    }

    const VirtualPagePayload* newest = nullptr;
    EXPECT_EQ(store.Fetch(base, 3, newest), VirtualGeometryPageStore::FetchState::Ready)
        << "the most recently staged page was discarded while older ones survived — the trim is "
           "evicting newest-first";
    EXPECT_LE(store.GetStats().PagesReady, 3u);
}

// A payload handed out by Fetch must survive until its Release, even while the cap is being
// enforced around it. TrimStagedLocked is reachable from Fetch, Poll and SetMaxStagedPages, so
// without a lease the header's "points at them until the matching Release" promise holds only
// by accident of which call happens next — and the failure would be a read of freed storage.
TEST_F(VirtualGeometryPageStoreTest, ALeasedPayloadSurvivesTheStagedCap)
{
    constexpr u32 kPages = 24;
    constexpr u32 kVertices = 16;
    const VirtualMeshGpuData source = MakePackedMesh(kPages, kVertices, 24, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    store.SetMaxStagedPages(2);
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    // Hold a lease on page 0...
    const VirtualPagePayload* leased = nullptr;
    ASSERT_EQ(FetchUntilSettled(store, base, 0, leased), VirtualGeometryPageStore::FetchState::Ready);
    ASSERT_NE(leased, nullptr);

    // ...then push far more pages through than the cap allows, so the trim runs repeatedly.
    //
    // FetchUntilSettled rather than a bare Fetch: whether any given Fetch happens to find its
    // page Ready depends on worker timing, and a loop built on that would only SOMETIMES push
    // the staged set over the cap — making the "the trim actually ran" assertion below a
    // coin flip in CI. Waiting for each page makes the overflow certain.
    for (u32 round = 0; round < 3; ++round)
    {
        for (u32 page = 1; page < kPages; ++page)
        {
            const VirtualPagePayload* other = nullptr;
            ASSERT_EQ(FetchUntilSettled(store, base, page, other),
                      VirtualGeometryPageStore::FetchState::Ready)
                << "page " << page << " never arrived in round " << round;
            // keepStaged, so these pile up in the staged set as UNLEASED entries — the only
            // kind the trim may evict. Releasing them outright would keep the set under the
            // cap and the trim would never run.
            store.Release(base, page, /*keepStaged=*/true);
        }
        store.Poll();
        store.SetMaxStagedPages(2); // also reaches the trim
    }

    // The lease is still valid and still holds page 0's bytes, not somebody else's.
    ExpectPageMatches(*leased, source, 0, /*withLightmapUVs=*/false);
    EXPECT_GT(store.GetStats().StagedDiscards, 0u)
        << "the trim never ran, so this test never exercised the case it exists for";
    store.Release(base, 0);
}

// Releasing with keepStaged leaves the bytes for the next attempt rather than re-reading them,
// but must still end the lease — a payload nobody released could never be trimmed, which would
// turn the "keep it for later" optimisation into an unbounded pin.
TEST_F(VirtualGeometryPageStoreTest, KeepStagedReleaseEndsTheLeaseAndAvoidsAReRead)
{
    const VirtualMeshGpuData source = MakePackedMesh(6, 12, 18, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    const VirtualPagePayload* payload = nullptr;
    ASSERT_EQ(FetchUntilSettled(store, base, 0, payload), VirtualGeometryPageStore::FetchState::Ready);
    u64 const readsAfterFirst = store.GetStats().ReadsIssued;
    store.Release(base, 0, /*keepStaged=*/true);

    // Still staged: the next Fetch is Ready immediately and costs no new read.
    const VirtualPagePayload* again = nullptr;
    EXPECT_EQ(store.Fetch(base, 0, again), VirtualGeometryPageStore::FetchState::Ready)
        << "keepStaged dropped the payload, so the page has to be read off disk again";
    EXPECT_EQ(store.GetStats().ReadsIssued, readsAfterFirst) << "the page was re-read despite being staged";
    ASSERT_NE(again, nullptr);
    ExpectPageMatches(*again, source, 0, /*withLightmapUVs=*/false);

    // And the lease really ended — a kept payload is trimmable like any other.
    store.Release(base, 0, /*keepStaged=*/true);
    store.SetMaxStagedPages(1);
    EXPECT_LE(store.GetStats().PagesReady, 1u);
}

// An out-of-range request is Failed, not Pending and not a zero-filled page. Pending would
// hang the caller's retry loop forever; a zero-filled page would be a degenerate cluster at
// the origin that draws.
TEST_F(VirtualGeometryPageStoreTest, RequestsOutsideTheDirectoryFailLoudly)
{
    const VirtualMeshGpuData source = MakePackedMesh(3, 8, 12, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);

    const VirtualPagePayload* payload = nullptr;
    EXPECT_EQ(store.Fetch(base, 99, payload), VirtualGeometryPageStore::FetchState::Failed);
    EXPECT_EQ(payload, nullptr);
    EXPECT_EQ(store.Fetch(VirtualGeometryPageStore::kInvalidMesh, 0, payload),
              VirtualGeometryPageStore::FetchState::Failed);

    VirtualPagePayload blocking;
    EXPECT_FALSE(store.ReadPageBlocking(base, 99, blocking));
    EXPECT_FALSE(store.ReadPageBlocking(VirtualGeometryPageStore::kInvalidMesh, 0, blocking));
}

// Two meshes must not alias. AddMesh hands out a base per mesh and the registry adds the
// page's index within its own mesh — an off-by-one in that arithmetic reads the neighbour's
// geometry, which renders perfectly and is wrong.
TEST_F(VirtualGeometryPageStoreTest, SeparateMeshesGetDisjointPageRanges)
{
    const VirtualMeshGpuData first = MakePackedMesh(3, 11, 15, /*withLightmapUVs=*/false);
    const VirtualMeshGpuData second = MakePackedMesh(5, 7, 9, /*withLightmapUVs=*/true);

    VirtualGeometryPageStore store;
    u32 const firstBase = store.AddMesh(first);
    u32 const secondBase = store.AddMesh(second);
    ASSERT_NE(firstBase, VirtualGeometryPageStore::kInvalidMesh);
    ASSERT_NE(secondBase, VirtualGeometryPageStore::kInvalidMesh);
    EXPECT_EQ(secondBase, firstBase + static_cast<u32>(static_cast<sizet>(first.Pages.Num())));

    for (u32 page = 0; page < static_cast<sizet>(first.Pages.Num()); ++page)
    {
        VirtualPagePayload payload;
        ASSERT_TRUE(store.ReadPageBlocking(firstBase, page, payload));
        ExpectPageMatches(payload, first, page, /*withLightmapUVs=*/false);
    }
    for (u32 page = 0; page < static_cast<sizet>(second.Pages.Num()); ++page)
    {
        VirtualPagePayload payload;
        ASSERT_TRUE(store.ReadPageBlocking(secondBase, page, payload));
        ExpectPageMatches(payload, second, page, /*withLightmapUVs=*/true);
    }
}

// A page window that runs past the packed arrays is refused OUTRIGHT, and the mesh keeps its
// in-memory payload. A cooked `.omesh` blob is hostile input — the registry deserializes one
// straight off disk — so a malformed page table must not become a heap read behind the
// vectors, and a partially-spilled mesh must not be left half-bound to the store.
TEST_F(VirtualGeometryPageStoreTest, PageWindowsOutsideThePackedArraysRefuseTheWholeMesh)
{
    VirtualMeshGpuData source = MakePackedMesh(3, 10, 20, /*withLightmapUVs=*/false);
    source.Pages[2].VertexCount += 5000; // past the end of Vertices

    VirtualGeometryPageStore store;
    EXPECT_EQ(store.AddMesh(source), VirtualGeometryPageStore::kInvalidMesh);
    // No half-written directory left behind: a later mesh must start at base 0.
    const VirtualMeshGpuData healthy = MakePackedMesh(2, 4, 6, /*withLightmapUVs=*/false);
    EXPECT_EQ(store.AddMesh(healthy), 0u);
}

// A spill file whose owning process was KILLED is collected by the next store that opens in
// the same directory. Close() only runs on a graceful shutdown, and these files are hundreds of
// megabytes — nine killed editor sessions left 4.1 GB in %TEMP% during this issue's measurement
// runs, which nothing would ever have reclaimed.
TEST_F(VirtualGeometryPageStoreTest, OpeningAStoreReclaimsSpillsFromDeadProcesses)
{
    std::error_code ec;
    std::filesystem::create_directories(m_Directory, ec);

    // PID 0 is never a live process on either platform, so this stands in for "the owner is
    // gone" without having to spawn and kill anything.
    auto const orphan = m_Directory / "vgpages_0_7.ovgp";
    {
        std::ofstream stream(orphan, std::ios::binary);
        stream << "dead session payload";
    }
    // ...and a file the sweep must NOT touch: it belongs to a process that is very much alive.
    // ...and a file the sweep must NOT touch, because it is not a spill at all.
    auto const live = m_Directory / "not-a-spill.txt";
    {
        std::ofstream stream(live, std::ios::binary);
        stream << "unrelated";
    }
    ASSERT_TRUE(std::filesystem::exists(orphan));

    const VirtualMeshGpuData source = MakePackedMesh(2, 4, 6, /*withLightmapUVs=*/false);
    VirtualGeometryPageStore store;
    ASSERT_NE(store.AddMesh(source), VirtualGeometryPageStore::kInvalidMesh);

    EXPECT_FALSE(std::filesystem::exists(orphan))
        << "a spill file owned by a dead process survived a new store opening — killed sessions leak "
           "hundreds of MB each";
    EXPECT_TRUE(std::filesystem::exists(live)) << "the sweep deleted a file that is not a spill";
    EXPECT_TRUE(std::filesystem::exists(store.GetPath())) << "the sweep deleted the store's own file";
}

// Close() joins the workers and removes the spill file. A session-scoped store that leaked
// its file would fill the temp drive one editor run at a time, on a box that also runs CI.
TEST_F(VirtualGeometryPageStoreTest, CloseJoinsWorkersAndDeletesTheSpillFile)
{
    const VirtualMeshGpuData source = MakePackedMesh(4, 16, 24, /*withLightmapUVs=*/false);

    VirtualGeometryPageStore store;
    u32 const base = store.AddMesh(source);
    ASSERT_NE(base, VirtualGeometryPageStore::kInvalidMesh);
    auto const path = store.GetPath();
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(std::filesystem::exists(path));

    // Leave a read in flight so Close has real work to join rather than an idle pool.
    const VirtualPagePayload* payload = nullptr;
    (void)store.Fetch(base, 0, payload);

    store.Close();
    EXPECT_FALSE(store.IsOpen());
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(store.GetStats().PagesWritten, 0u);

    // Idempotent — Shutdown can run twice, and the destructor runs after it.
    store.Close();
}
