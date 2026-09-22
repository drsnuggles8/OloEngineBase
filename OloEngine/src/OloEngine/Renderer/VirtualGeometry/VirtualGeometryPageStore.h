#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <glm/vec2.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <array>
#include "OloEngine/Containers/Deque.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    // @brief The on-disk backing store beneath VirtualMeshRegistry::LoadPage (issue #1151).
    //
    // Until this existed, "streaming" virtual geometry meant CPU-resident payload -> VRAM
    // residency: the registry's slot cache paged a mesh's geometry into budgeted GPU arenas,
    // but the source it paged FROM was `MeshEntry::Packed` — the whole cooked payload, held in
    // RAM for the life of the process. The VRAM budget was real and the RAM ceiling was
    // "every virtualized mesh's full payload fits", which is far below it. The point of
    // virtualized geometry is scenes larger than memory, so the missing layer was the one
    // that faults a page in from DISK.
    //
    // This is that layer, and it is deliberately only that: it knows how to write page
    // payloads to a file and read them back, and nothing about residency, LRU, pinning or
    // GPU arenas. The registry's existing machinery (slot cache, eviction listener, readback
    // ring) is untouched and still owns every residency decision.
    //
    // ## Why a session spill rather than a page-addressable `.omesh`
    //
    // The structural choice was "page-addressable sidecar vs. whole blob with an index".
    // Neither, in the end: the store is written from the PACKED data at pool-rebuild time,
    // into a process-private spill file, and the packed geometry arrays are then dropped.
    //
    // The reason is that a cooked `.omesh` blob is not page-addressable today and making it
    // so is a format change that reaches the cook, the importer, the asset pack and every
    // cache already on disk — and it would still not remove the peak this store removes,
    // because `RegisterMeshSource` materialises the DAG (clusters, groups, page table) from
    // the blob regardless. What the spill buys is the STEADY-STATE ceiling, which is the one
    // that decides whether a scene fits: after the rebuild, resident RAM is cluster/group
    // metadata plus whatever is in flight, and the per-mesh geometry is gone. The remaining
    // peak is one mesh's payload at a time, not the scene's.
    //
    // A session spill also has no staleness class at all. A persistent store would need a
    // cook fingerprint, an invalidation rule and a cache-eviction policy, and a stale one
    // would feed the GPU another build's vertices — self-consistently, which is exactly the
    // failure mode kVirtualMeshBuilderVersion exists to prevent (VirtualMesh.h).
    //
    // ## Threading
    //
    // AddMesh / Fetch / Release / ReadPageBlocking are RENDER-THREAD ONLY. The worker threads
    // only ever pop a request, read bytes at an offset, and push the result back; they never
    // touch registry state. Each worker owns its own read handle, so seeks never contend.

    // One page's geometry, as read back out of the store. Page-LOCAL: index 0 is the page's
    // first vertex, not the mesh's, which is what lets the registry's slot rebasing stay
    // exactly what it was for the in-memory path.
    struct VirtualPagePayload
    {
        TArray<VirtualGpuVertex> Vertices;
        TArray<glm::vec2> LightmapUVs; // empty when the mesh carries no baked uv2
        TArray<u32> Indices;

        [[nodiscard]] u64 ByteSize() const
        {
            return static_cast<u64>(Vertices.Num()) * sizeof(VirtualGpuVertex) + static_cast<u64>(LightmapUVs.Num()) * sizeof(glm::vec2) +
                   static_cast<u64>(Indices.Num()) * sizeof(u32);
        }
    };

    // Everything the store can say about itself, so a stall or a bad read is a NUMBER rather
    // than a silence. Surfaced through VirtualResidencyStats into the editor's Statistics
    // panel and olo_virtual_geometry_stats.
    struct VirtualPageStoreStats
    {
        u64 PagesWritten = 0;
        u64 BytesWritten = 0;
        u64 ReadsIssued = 0;    // async fetches handed to a worker
        u64 ReadsCompleted = 0; // ...of which came back with bytes
        u64 ReadsFailed = 0;    // ...of which came back short, or could not be opened/seeked
        u64 BlockingReads = 0;  // pinned-page / eager loads that bypassed the workers
        u64 BytesRead = 0;
        u32 ReadsInFlight = 0; // queued or being served right now
        u32 PagesReady = 0;    // read, waiting for the next LoadPage to consume them
        // Ready payloads dropped because the staged set hit its cap before anyone consumed
        // them. Normal in small numbers — a page requested on one frame can stop being
        // requested before it lands — but a large or fast-growing count means the residency
        // pass is reading far more than it uploads, i.e. the camera is outrunning the disk.
        u64 StagedDiscards = 0;
        // RAM held by payloads that have been READ and not yet consumed. A read still in
        // flight is not counted — its buffer lives on the worker's stack until it completes,
        // so the store cannot size it yet; that part of the footprint is bounded separately,
        // at one page per outstanding read (ReadsInFlight <= MaxReadsInFlight).
        u64 StagingBytes = 0;
        u64 PeakStagingBytes = 0;
    };

    class VirtualGeometryPageStore
    {
      public:
        static constexpr u32 kInvalidMesh = 0xFFFFFFFFu;

        VirtualGeometryPageStore() = default;
        ~VirtualGeometryPageStore();

        VirtualGeometryPageStore(const VirtualGeometryPageStore&) = delete;
        VirtualGeometryPageStore& operator=(const VirtualGeometryPageStore&) = delete;

        // What a Fetch found. Pending is the honest answer for "the bytes are not here yet";
        // the caller must leave the page non-resident and try again, never draw as if the
        // page were empty.
        enum class FetchState : u8
        {
            Ready = 0,
            // A read for this page is outstanding. The caller should treat the page as work
            // already started and come back for it.
            Pending = 1,
            // No read is outstanding and none was started: the in-flight cap is full. Distinct
            // from Pending because a caller that budgets its per-frame work must not spend
            // budget on a page it did not actually get a read for — with the cap and the
            // budget sized alike, doing so lets one frame burn its whole budget on refusals
            // and visit none of the pages that are already Ready.
            Deferred = 2,
            Failed = 3
        };

        // Spills every page of `packed` and returns the store-local base index for this mesh
        // (page i of the mesh is base + i), or kInvalidMesh when the file could not be opened
        // or the write failed. A caller that gets kInvalidMesh must KEEP its in-memory copy —
        // there is no other source for those bytes.
        [[nodiscard]] u32 AddMesh(const VirtualMeshGpuData& packed);

        // Synchronous read, for the pages that have no fallback to wait on: the pinned
        // (terminal) pages that guarantee a drawable cut, and eager residency where every
        // page is loaded at rebuild anyway.
        [[nodiscard]] bool ReadPageBlocking(u32 meshBase, u32 pageIndex, VirtualPagePayload& out);

        // Asynchronous fault-in. On the first call for a page this queues a worker read and
        // answers Pending; once the bytes are in, it answers Ready and `outPayload` points at
        // them until the matching Release. A page that has already failed answers Failed
        // forever — a bad read is not retried into an infinite request loop.
        //
        // With no IO worker (every read handle failed to open — StartWorkers logs that as an
        // error) this serves the read SYNCHRONOUSLY on the calling thread instead. That is a
        // real cost and it is stated in the log and counted in BlockingReads; answering
        // Pending forever would instead leave every non-pinned page non-resident for the
        // session with all three read counters reading zero.
        [[nodiscard]] FetchState Fetch(u32 meshBase, u32 pageIndex, const VirtualPagePayload*& outPayload);

        // Ends the lease Fetch handed out, so the pointer must not be used afterwards.
        //
        // keepStaged = false drops the payload and reclaims its staging memory. true keeps the
        // bytes for a later attempt, which is what a caller wants when it got the page but
        // could not use it yet (no cache slot free this frame) — re-reading the identical page
        // off disk every frame is pure read amplification. Either way the entry becomes
        // trimmable again; a kept payload is subject to the staged cap like any other.
        //
        // Safe to call for a page that is not staged.
        void Release(u32 meshBase, u32 pageIndex, bool keepStaged = false);

        // Collects finished reads and re-applies the staged cap without asking for anything.
        // Needed because every other path into the completion queue runs only while pages are
        // being requested: when the camera stops (or residency is complete and the caller
        // early-outs), the last reads would otherwise stay "in flight" in the stats forever
        // and their payloads stay allocated. Cheap — a lock and a queue splice.
        void Poll();

        // Ceiling on queued + in-flight reads. Clamped to at least 1: a zero would wedge the
        // fault-in path silently.
        void SetMaxReadsInFlight(u32 maxReads);
        [[nodiscard]] u32 GetMaxReadsInFlight() const
        {
            return m_MaxReadsInFlight;
        }
        // Ceiling on payloads that have been READ but not yet consumed, which is the other
        // half of the staging bound and the one that is easy to miss: capping only the reads
        // in flight bounds nothing, because a page whose group stopped being requested before
        // it landed is never consumed and never released. Oldest-first discard past the cap.
        void SetMaxStagedPages(u32 maxStaged);
        [[nodiscard]] u32 GetMaxStagedPages() const
        {
            return m_MaxStagedPages;
        }

        [[nodiscard]] bool IsOpen() const
        {
            return m_File != nullptr;
        }
        [[nodiscard]] const VirtualPageStoreStats& GetStats() const
        {
            return m_Stats;
        }
        [[nodiscard]] const std::filesystem::path& GetPath() const
        {
            return m_Path;
        }

        // Joins the workers, closes the file and deletes it. Idempotent.
        void Close();

        // Directory the spill file is created in. Defaults to
        // <temp>/OloEngine/VirtualGeometryPages. Tests point it at their own scratch
        // directory; an empty path restores the default. Takes effect on the next store that
        // opens a file, not on one already open.
        static void SetSpillDirectory(const std::filesystem::path& directory);
        [[nodiscard]] static std::filesystem::path GetSpillDirectory();

      private:
        // The file-resident extent of one page. VertexCount/IndexCount are kept rather than
        // only byte counts because the reader sizes its vectors from them, and a count that
        // disagrees with the stored byte span is the corruption this is meant to catch.
        struct PageRecord
        {
            u64 Offset = 0;
            u32 VertexCount = 0;
            u32 IndexCount = 0;
            u32 LightmapUVCount = 0; // 0 = this mesh carried no uv2
            u32 Pad0 = 0;
        };

        struct Request
        {
            u32 Page = 0; // index into m_Directory
        };

        template<typename>
        friend struct TIsTriviallyRelocatable;

        struct Completion
        {
            u32 Page = 0;
            bool Ok = false;
            VirtualPagePayload Payload;
        };

        [[nodiscard]] bool EnsureOpen();
        void StartWorkers();
        // Reads one directory entry into `out` through `stream`. Static so both the workers
        // (own handle) and the blocking path (the write handle, re-opened for read) share one
        // implementation — a second copy of the offset arithmetic is exactly where a
        // corruption bug would hide.
        [[nodiscard]] static bool ReadRecord(std::FILE* stream, const PageRecord& record, VirtualPagePayload& out);
        void WorkerMain(std::FILE* stream);
        // Moves whatever the workers have finished into m_Ready / m_Failed. Must be called
        // with m_Mutex held.
        void DrainCompletionsLocked();
        void NoteStagingBytesLocked(i64 delta);
        // Takes ownership of a freshly-read payload and records its arrival order. Must be
        // called with m_Mutex held.
        void StageLocked(u32 page, VirtualPagePayload&& payload);
        // Drops the oldest ready payloads until the staged set is within m_MaxStagedPages.
        // Must be called with m_Mutex held.
        void TrimStagedLocked();

        std::filesystem::path m_Path;
        std::FILE* m_File = nullptr; // append/read handle owned by the render thread
        u64 m_WriteCursor = 0;
        bool m_OpenFailed = false; // a failed open is reported once, then remembered

        TArray<PageRecord> m_Directory;
        VirtualPageStoreStats m_Stats;
        u32 m_MaxReadsInFlight = 32;
        u32 m_MaxStagedPages = 64;

        // Worker plumbing. m_Mutex guards m_Directory, m_Queue, m_Completions, m_Requested,
        // m_Ready, m_Failed and m_Stats; m_Path, m_File, m_WriteCursor and the worker vectors
        // are render-thread-only and are not touched while a worker can observe them.
        // OS thread objects have fixed addresses; they never enter relocatable storage.
        static constexpr u32 kWorkerCount = 2;
        std::array<std::thread, kWorkerCount> m_Workers;
        u32 m_WorkerCount = 0;
        TArray<std::FILE*> m_WorkerFiles;
        std::mutex m_Mutex;
        std::condition_variable m_WorkAvailable;
        TDeque<Request> m_Queue;
        TArray<Completion> m_Completions;
        std::unordered_set<u32> m_Requested; // queued or being served
        std::atomic<bool> m_Stop{ false };

        // One staged payload, plus the arrival number that identifies THIS staging of it.
        // The sequence number is what makes the order queue safe against a page that is
        // released and later staged again: without it the queue holds the same key twice, the
        // stale front entry still names a live map entry, and the oldest-first trim deletes
        // the NEWEST payload — a page reached first each frame could then be read, trimmed and
        // re-read forever without becoming resident.
        struct ReadyEntry
        {
            VirtualPagePayload Payload;
            u64 Seq = 0;
            // A Fetch handed a pointer to this payload out and the caller has not Released it
            // yet. The trim must not erase it: Fetch's contract is that the pointer stays
            // valid until Release, and TrimStagedLocked is reachable from Fetch, Poll and
            // SetMaxStagedPages, so without this the promise is only true by accident of who
            // happens to call what. Leases are short — the registry finishes with a payload
            // inside the same LoadPage call — so this pins at most a handful of pages.
            bool Leased = false;
        };

        // Completed reads, and the pages whose read failed. A payload handed out by Fetch
        // stays valid until the matching Release: unordered_map never invalidates references
        // on rehash, and nothing else can touch an entry that is already Ready (a page in
        // m_Ready is not in m_Requested, so no completion can overwrite it).
        std::unordered_map<u32, ReadyEntry> m_Ready;
        // Arrival order as (page, seq) pairs. An entry whose page is gone from m_Ready, or
        // whose seq no longer matches the live one, is stale and is skipped.
        struct ReadyOrderEntry
        {
            u32 Page = 0;
            u64 Seq = 0;
        };
        TDeque<ReadyOrderEntry> m_ReadyOrder;
        u64 m_NextReadySeq = 1;
        std::unordered_set<u32> m_Failed;
    };

    template<>
    struct TIsTriviallyRelocatable<VirtualPagePayload>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(VirtualPagePayload::Vertices)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualPagePayload::LightmapUVs)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualPagePayload::Indices)>::Value;
    };

    template<>
    struct TIsTriviallyRelocatable<VirtualGeometryPageStore::Completion>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(VirtualGeometryPageStore::Completion::Page)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualGeometryPageStore::Completion::Ok)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualGeometryPageStore::Completion::Payload)>::Value;
    };
} // namespace OloEngine
